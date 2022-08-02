// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <torch/extension.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/FunctionalTensorWrapper.h>

#include <functorch/csrc/TensorWrapper.h>
#include <functorch/csrc/DynamicLayer.h>
#include <functorch/csrc/BatchedTensorImpl.h>
#include <functorch/csrc/LegacyVmapTransforms.h>
#include <functorch/csrc/BatchedFallback.h>
#include <functorch/csrc/BatchRulesHelper.h>
#include <functorch/csrc/CompileCache.h>
#include <functorch/csrc/CustomFunction.h>
#include <c10/core/AutogradState.h>
#include <functorch/csrc/dim/dim.h>

namespace at {
namespace functorch {

static bool has_level(const Tensor& self, int64_t level) {
  const auto* batched = maybeGetBatchedImpl(self);
  if (!batched) {
    return false;
  }
  return batched->level() >= level;
}

Tensor _add_batch_dim(const Tensor& self, int64_t batch_dim, int64_t level) {
  return addBatchDim(self, batch_dim, level);
}

Tensor _wrap_functional_tensor(const Tensor& self, int64_t level) {
  auto t = at::functionalization::impl::to_functional_tensor(self);
  at::functionalization::impl::unsafeGetFunctionalWrapper(t)->set_level(level);
  return t;
}

void _assert_wrapped_functional(const Tensor& unwrapped, const Tensor& wrapped) {
  TORCH_INTERNAL_ASSERT(at::functionalization::impl::isFunctionalTensor(wrapped));
  TORCH_INTERNAL_ASSERT(!at::functionalization::impl::isFunctionalTensor(unwrapped));
  auto wrapped_impl = at::functionalization::impl::unsafeGetFunctionalWrapper(wrapped);
  auto& wrapped_inner = wrapped_impl->value();
  TORCH_INTERNAL_ASSERT(unwrapped.unsafeGetTensorImpl() == wrapped_inner.unsafeGetTensorImpl())
}

void _propagate_functional_input_mutation(const Tensor& unwrapped, const Tensor& wrapped) {
  TORCH_INTERNAL_ASSERT(at::functionalization::impl::isFunctionalTensor(wrapped));
  TORCH_INTERNAL_ASSERT(!at::functionalization::impl::isFunctionalTensor(unwrapped));
  auto wrapped_impl = at::functionalization::impl::unsafeGetFunctionalWrapper(wrapped);
  // Ensure that the input is up to date by committing any pending updates to the alias.
  wrapped_impl->sync_();
  auto& wrapped_inner = wrapped_impl->value();
  // It would probably be more reasonable to check that the two tensors are aliased,
  // but we can't do that unless we give BatchedTensorImpl a notion of storage.
  if (unwrapped.unsafeGetTensorImpl() == wrapped_inner.unsafeGetTensorImpl()) {
  } else {
      TORCH_INTERNAL_ASSERT(unwrapped.nbytes() == wrapped_inner.nbytes());
      TORCH_INTERNAL_ASSERT(unwrapped.sizes() == wrapped_inner.sizes(),
          "An inplace-mutation op (like transpose_() was called on an input to the functionalization pass."
          " Propagating those mutations to the input is currently not supported.");
      unwrapped.copy_(wrapped_inner);
  }
}


static std::pair<Tensor,int64_t> remove_existing_batch_dim(
    const BatchedTensorImpl* batched, int64_t level) {

  TORCH_INTERNAL_ASSERT(batched->level() == level);
  return std::make_pair(batched->value(), batched->bdim());
}

// Poor man's version of np.moveaxis. Moves the dimension at `dst` to `src`
// while preserving the order of other existing dimensions.
// We should probably add np.moveaxis (it is more general) to PyTorch. (#36048)
// When we do, replace the following with it.
static Tensor _movedim(const Tensor& self, int64_t src, int64_t dst) {
  auto logical_dim = self.dim();
  src = maybe_wrap_dim(src, logical_dim);
  dst = maybe_wrap_dim(dst, logical_dim);
  if (src == dst) {
    return self;
  }
  VmapDimVector permutation;
  permutation.reserve(logical_dim);
  for (int64_t dim = 0; dim < logical_dim; dim++) {
    if (dim == src) {
      continue;
    }
    permutation.push_back(dim);
  }
  permutation.insert(permutation.begin() + dst, src);
  return self.permute(permutation);
}

// Removes the batch dim with level `level` from `self`. If this causes the
// last batch dim to be removed from a BatchedTensor, then this returns a
// regular Tensor.
//
// If the `level` of the batch dim to remove does not exist in `self`, then we
// add the batch dim in. This can happen if `self` didn't interact with a tensor
// inside the vmap level, for example,
//     self = torch.randn(3)
//     y = torch.randn(5)
//     out = vmap(lambda x: vmap(lambda y: x)(y))(self)
//     assert out.shape == (3, 5)
// Inside the inner vmap, `x` is a BatchedTensor with a single batch dimension
// corresponding to the *outer* vmap level and it doesn't have any dimensions that
// correspond to the inner vmap level so we need to create one for the user.
//
// `out_dim` controls where we should put the batch dimension in the output tensor.
Tensor _remove_batch_dim(const Tensor& self, int64_t level, int64_t batch_size, int64_t out_dim) {
  if (!has_level(self, level)) {
    auto self_sizes = self.sizes();
    VmapDimVector expanded_sizes(self_sizes.begin(), self_sizes.end());
    expanded_sizes.insert(expanded_sizes.begin() + out_dim, batch_size);
    auto result = self.expand(expanded_sizes);
    return result;
  }

  // Must be batched if has_level(self, /*any_level*/)
  const auto* batched = maybeGetBatchedImpl(self);
  TORCH_INTERNAL_ASSERT(batched != nullptr);

  Tensor self_without_bdim;
  int64_t newly_exposed_logical_dim;
  std::tie(self_without_bdim, newly_exposed_logical_dim) = remove_existing_batch_dim(batched, level);
  auto result = _movedim(self_without_bdim, newly_exposed_logical_dim, out_dim);
  return result;
}

Tensor _unwrap_functional_tensor(const Tensor& self, bool add_back_views) {
  // We only ever call that after popping out of a functionalize() call, in which case the current tensors
  // should always be wrapped in a FunctionalTensorWrapper.
  TORCH_INTERNAL_ASSERT(at::functionalization::impl::isFunctionalTensor(self));
  auto functional = at::functionalization::impl::unsafeGetFunctionalWrapper(self);

  // when regenerating the (potentially mutated) input tensors, the functionalization pass
  // regenerates them through a series of view_copy() op calls.
  // Functorch wants to turn those back into view ops though.
  // Ensure that the input is up to date by committing any pending updates to the alias.
  at::functionalization::impl::FunctionalizationReapplyViewsGuard guard(add_back_views);
  bool any_updates = functional->apply_updates();
  if (any_updates) {
    functional->regenerate_from_base();
  }
  return functional->value();
}

Tensor _wrap_for_grad(const Tensor& self, int64_t level) {
  // NB: different behavior inside??
  // return self;
  // TORCH_INTERNAL_ASSERT(!maybeGetTensorWrapper(self));
  // TORCH_INTERNAL_ASSERT(self.has_storage());
  return makeTensorWrapper(self, level);
}

Tensor _unwrap_for_grad(const Tensor& self, int64_t level) {
  auto* result = maybeGetTensorWrapper(self);
  if (!result) {
    return self;
  }
  TORCH_INTERNAL_ASSERT(result->level().has_value());
  if (result->level() == level) {
    return result->value();
  }
  return self;
}

int64_t dlevel(const Tensor& tensor) {
  auto* wrapped = maybeGetTensorWrapper(tensor);
  if (!wrapped) {
    return 0;
  }
  if (!wrapped->is_alive()) {
    return -1;
  }
  return wrapped->level().value();
}

bool dump_tensor(const Tensor& self) {
  dumpTensorCout(self);
  return true;
}

RandomnessType get_randomness_enum(const std::string& randomness) {
    if (randomness == "error") {
        return RandomnessType::Error;
    } else if (randomness == "same") {
        return RandomnessType::Same;
    } else if (randomness == "different") {
        return RandomnessType::Different;
    } else {
        TORCH_CHECK(false, "randomness argument must be error, same, or different.");
    }
}

void set_fwd_grad_enabled(bool enabled) {
  AutogradState::get_tls_state().set_fw_grad_mode(enabled);
}

bool get_fwd_grad_enabled() {
  return AutogradState::get_tls_state().get_fw_grad_mode();
}

int64_t _grad_increment_nesting() {
  // See NOTE [grad and vjp interaction with no_grad]
  bool prev_grad_mode = c10::GradMode::is_enabled();
  return initAndPushDynamicLayer(TransformType::Grad, nullopt, nullopt, prev_grad_mode);
}

int64_t _grad_decrement_nesting() {
  auto layer = popDynamicLayerAndDeleteMetadata();
  TORCH_INTERNAL_ASSERT(layer.key() == TransformType::Grad);
  return layer.layerId();
}

int64_t _jvp_increment_nesting() {
  // See NOTE [grad and vjp interaction with no_grad]
  bool prev_fwd_grad_mode = get_fwd_grad_enabled();
  return initAndPushDynamicLayer(TransformType::Jvp, nullopt, nullopt, nullopt, prev_fwd_grad_mode);
}

int64_t _jvp_decrement_nesting() {
  auto layer = popDynamicLayerAndDeleteMetadata();
  TORCH_INTERNAL_ASSERT(layer.key() == TransformType::Jvp);
  return layer.layerId();
}

int64_t _vmap_increment_nesting(int64_t batch_size, const std::string& randomness) {
  return initAndPushDynamicLayer(TransformType::Vmap, batch_size, get_randomness_enum(randomness));
}

int64_t _vmap_decrement_nesting() {
  auto layer = popDynamicLayerAndDeleteMetadata();
  TORCH_INTERNAL_ASSERT(layer.key() == TransformType::Vmap);
  return layer.layerId();
}

int64_t _func_increment_nesting(bool reapply_views) {
  return initAndPushDynamicLayer(TransformType::Functionalize, c10::nullopt, c10::nullopt, c10::nullopt, c10::nullopt, /*functionalize_add_back_views=*/reapply_views);
}

int64_t _func_decrement_nesting() {
  auto layer = popDynamicLayerAndDeleteMetadata();
  TORCH_INTERNAL_ASSERT(layer.key() == TransformType::Functionalize);
  return layer.layerId();
}

static bool is_batchedtensor(const Tensor& tensor) {
  auto* batched = maybeGetBatchedImpl(tensor);
  return batched != nullptr;
}

static bool is_gradtrackingtensor(const Tensor& tensor) {
  auto* wrapped = maybeGetTensorWrapper(tensor);
  return wrapped != nullptr;
}

static bool is_functionaltensor(const Tensor& tensor) {
  return tensor.unsafeGetTensorImpl()->key_set().has(c10::DispatchKey::Functionalize);
}

static Tensor get_unwrapped(const Tensor& tensor) {
  auto* batched = maybeGetBatchedImpl(tensor);
  if (batched) {
    return batched->value();
  }
  auto* wrapped = maybeGetTensorWrapper(tensor);
  if (wrapped) {
    return wrapped->value();
  }
  auto* functional = dynamic_cast<FunctionalTensorWrapper*>(tensor.unsafeGetTensorImpl());
  if (functional) {
    return functional->value();
  }
  TORCH_CHECK(false, "No wrappers present!");
}

static int64_t maybe_get_level(const Tensor& tensor) {
  auto* batched = maybeGetBatchedImpl(tensor);
  if (batched) {
    return batched->level();
  }
  auto* wrapped = maybeGetTensorWrapper(tensor);
  if (wrapped) {
    if (wrapped->level()) {
      return *wrapped->level();
    }
    // TODO: this is a weird special case...
    return -2;
  }
  auto* functional = dynamic_cast<FunctionalTensorWrapper*>(tensor.unsafeGetTensorImpl());
  if (functional) {
      return functional->level();
  }
  return -1;
}

static int64_t maybe_get_bdim(const Tensor& tensor) {
  auto* batched = maybeGetBatchedImpl(tensor);
  if (batched) {
    return batched->bdim();
  }
  return -1;
}

static int64_t currentLevel() {
  auto maybe_layer = maybeCurrentDynamicLayer();
  TORCH_INTERNAL_ASSERT(maybe_layer.has_value());
  int64_t current_level = maybe_layer->layerId();
  return current_level;
}

static std::tuple<Tensor, int64_t> unwrapTensorAtCurrentLevel(const Tensor& tensor) {
  auto maybe_layer = maybeCurrentDynamicLayer();
  TORCH_INTERNAL_ASSERT(maybe_layer.has_value());
  int64_t current_level = maybe_layer->layerId();
  auto result = unwrapTensorAtLevel(tensor, current_level);
  auto value = std::get<0>(result);
  auto bdim = std::get<1>(result);
  value = moveBatchDimToFront(value, bdim);
  return std::make_tuple(value, bdim.has_value() ? 0 : -1);
}

static void tls_set_vmap_excluded(bool excluded) {
  c10::impl::tls_set_dispatch_key_excluded(kBatchedKey, excluded);
}

static bool tls_set_is_included() {
  return c10::impl::tls_is_dispatch_key_included(kDynamicLayerFrontModeKey);
}

static void _set_dynamic_layer_keys_included(bool value) {
  return setDynamicLayerFrontBackKeysIncluded(value);
}

static void dump_dls() {
  std::cout << getDynamicLayerStack() << std::endl;
}

static void dump_local_tls() {
  auto tls = c10::impl::tls_local_dispatch_key_set();
  std::cout << "[Local Include] " << tls.included_ << std::endl;
  std::cout << "[Local Exclude] " << tls.excluded_ << std::endl;
}

} // namespace functorch
}


namespace at { namespace functorch {

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("_add_batch_dim", &at::functorch::_add_batch_dim, "add batch dim");
  m.def("_remove_batch_dim", &at::functorch::_remove_batch_dim, "remove batch dim");
  m.def("_wrap_functional_tensor", &at::functorch::_wrap_functional_tensor, "add functional tensor");
  m.def("_assert_wrapped_functional", &at::functorch::_assert_wrapped_functional, "assert wrapped functional");
  m.def("_propagate_functional_input_mutation", &at::functorch::_propagate_functional_input_mutation, "propagate functional input mutations");
  m.def("_unwrap_functional_tensor", &at::functorch::_unwrap_functional_tensor, "remove functional tensor");
  m.def("_vmap_increment_nesting", &at::functorch::_vmap_increment_nesting, "remove batch dim");
  m.def("_vmap_decrement_nesting", &at::functorch::_vmap_decrement_nesting, "remove batch dim");
  m.def("_func_increment_nesting", &at::functorch::_func_increment_nesting, "functionalization start");
  m.def("_func_decrement_nesting", &at::functorch::_func_decrement_nesting, "functionalization end");
  m.def("_grad_increment_nesting", &at::functorch::_grad_increment_nesting, "remove batch dim");
  m.def("_grad_decrement_nesting", &at::functorch::_grad_decrement_nesting, "remove batch dim");
  m.def("_jvp_increment_nesting", &at::functorch::_jvp_increment_nesting);
  m.def("_jvp_decrement_nesting", &at::functorch::_jvp_decrement_nesting);
  m.def("_wrap_for_grad", &at::functorch::_wrap_for_grad, "wrap as gradtrackingtensor");
  m.def("_unwrap_for_grad", &at::functorch::_unwrap_for_grad, "unwrap from gradtrackingtensor");
  m.def("_set_vmap_fallback_warning_enabled", &at::functorch::setVmapFallbackWarningEnabled, "Set vmap fallback warnings");
  m.def("_set_vmap_fallback_enabled", &at::functorch::setVmapFallbackEnabled);
  m.def("_is_vmap_fallback_enabled", &at::functorch::isVmapFallbackEnabled);
  m.def("set_inplace_requires_grad_allowed", &at::functorch::setInplaceRequiresGradAllowed);
  m.def("get_inplace_requires_grad_allowed", &at::functorch::getInplaceRequiresGradAllowed);
  m.def("dlevel", &at::functorch::dlevel, "dlevel");
  m.def("dump_tensor", &at::functorch::dump_tensor, "dump_tensor");
  m.def("reshape_dim_into", &at::functorch::reshape_dim_into);
  m.def("reshape_dim_outof", &at::functorch::reshape_dim_outof);
  m.def("are_transforms_active", &at::functorch::areTransformsActive);
  // various debugging things. Maybe we should offer these as first-class APIs
  // on Tensors?
  m.def("is_batchedtensor", &at::functorch::is_batchedtensor);
  m.def("is_gradtrackingtensor", &at::functorch::is_gradtrackingtensor);
  m.def("is_functionaltensor", &at::functorch::is_functionaltensor);
  m.def("get_unwrapped", &at::functorch::get_unwrapped);
  m.def("maybe_get_level", &at::functorch::maybe_get_level);
  m.def("maybe_get_bdim", &at::functorch::maybe_get_bdim);
  m.def("current_level", &at::functorch::currentLevel);
  m.def("unwrap_batchedtensor", &at::functorch::unwrapTensorAtCurrentLevel);
  m.def("tls_set_vmap_excluded", &at::functorch::tls_set_vmap_excluded);
  m.def("tls_set_is_included", &at::functorch::tls_set_is_included);
  m.def("_set_dynamic_layer_keys_included", &at::functorch::_set_dynamic_layer_keys_included);
  m.def("dump_dls", &at::functorch::dump_dls);
  m.def("dump_local_tls", &at::functorch::dump_local_tls);
  m.def("set_fwd_grad_enabled", &at::functorch::set_fwd_grad_enabled);
  m.def("get_fwd_grad_enabled", &at::functorch::get_fwd_grad_enabled);

  at::functorch::initCompileCacheBindings(m.ptr());

  // initialize first-class dims and install it as a submodule on _C
  auto dim = Dim_init();
  if (!dim) {
    throw py::error_already_set();
  }
  py::setattr(m, "dim", py::reinterpret_steal<py::object>(dim));

  // Windows doesn't like this
#ifndef _WIN32
  initDispatchBindings(m.ptr());
#endif
}

}}
