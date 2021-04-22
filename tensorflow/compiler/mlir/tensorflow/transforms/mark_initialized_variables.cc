/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/compiler/mlir/tensorflow/transforms/mark_initialized_variables.h"

#include <string>
#include <vector>

#include "mlir/IR/Block.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_n_z.h"
#include "tensorflow/compiler/mlir/utils/string_container_utils.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/framework/resource_var.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/public/session.h"

namespace mlir {
namespace tf_saved_model {

// Returns true if the variable 'var_handle_op' is initialized in 'session'.
bool IsVariableInitialized(mlir::TF::VarHandleOp var_handle_op,
                           llvm::StringRef device_name,
                           const tensorflow::DeviceMgr* mgr,
                           tensorflow::Session* session) {
  tensorflow::Device* device = nullptr;
  if (!mgr || !mgr->LookupDevice(StringRefToView(device_name), &device).ok())
    return false;
  tensorflow::Var* var_ptr = nullptr;
  const auto& container = var_handle_op.container().str();
  auto status = device->resource_manager()->Lookup(
      (container.empty() ? device->resource_manager()->default_container()
                         : container),
      var_handle_op.shared_name().str(), &var_ptr);
  if (!device || !status.ok()) return false;
  auto* tensor = var_ptr->tensor();
  bool is_initialized = tensor && tensor->IsInitialized();
  var_ptr->Unref();
  return is_initialized;
}

LogicalResult MarkInitializedVariablesInFunction(FuncOp function,
                                                 tensorflow::Session* session,
                                                 mlir::MLIRContext* context) {
  if (!session || !llvm::hasSingleElement(function)) return success();
  Block& block = function.front();

  // Fetch all variable in one session run call.
  std::vector<std::string> variables;
  std::vector<TF::VarHandleOp> var_ops;
  for (auto var_handle_op : block.getOps<TF::VarHandleOp>()) {
    // In some cases the shared_name attribute doesn't have the same
    // tensor name in the model, so we first try to use the location
    // then fallback to shared_name attribute.
    if (auto loc = var_handle_op->getLoc().dyn_cast<NameLoc>()) {
      variables.push_back(loc.getName().str());
    } else {
      variables.push_back(var_handle_op.shared_name().str());
    }
    var_ops.push_back(var_handle_op);
  }
  if (variables.empty()) return success();

  std::vector<tensorflow::Tensor> resource_tensors;
  auto status = session->Run({}, variables, {}, &resource_tensors);
  if (!status.ok()) {
    return function->emitError("failed to run Session: " +
                               status.error_message());
  }

  const tensorflow::DeviceMgr* mgr = nullptr;
  status = session->LocalDeviceManager(&mgr);
  if (!status.ok())
    return function->emitError("failed to fetch device manager: " +
                               status.error_message());
  for (auto var_and_tensor : llvm::zip(var_ops, resource_tensors)) {
    auto& var_op = std::get<0>(var_and_tensor);
    auto& resource_tensor = std::get<1>(var_and_tensor);
    bool is_variable_initialized = false;
    if (resource_tensor.dtype() != tensorflow::DT_RESOURCE) {
      is_variable_initialized = true;
    } else {
      auto handle = resource_tensor.scalar<tensorflow::ResourceHandle>()();
      is_variable_initialized =
          IsVariableInitialized(var_op, handle.device(), mgr, session);
    }
    var_op->setAttr("_is_initialized",
                    BoolAttr::get(context, is_variable_initialized));
  }
  return success();
}
}  // namespace tf_saved_model
}  // namespace mlir
