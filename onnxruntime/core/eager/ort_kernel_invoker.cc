// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/eager/ort_kernel_invoker.h"
#include "core/optimizer/optimizer_execution_frame.h"
#include "core/common/logging/logging.h"
#include "core/graph/model.h"
#include "core/framework/op_kernel.h"
#include "core/session/ort_env.h"
#include "core/graph/constants.h"

namespace onnxruntime {

#define ORT_EAGER_ONNX_OPSET_VERSION 14

common::Status ORTInvoker::Invoke(const std::string& op_name,
                                  //optional inputs / outputs?
                                  const std::vector<OrtValue>& inputs,
                                  std::vector<OrtValue>& outputs,
                                  const NodeAttributes* attributes,
                                  const std::string& domain,
                                  const int version) {
  std::unordered_map<std::string, int> domain_version_map = {{kOnnxDomain, ORT_EAGER_ONNX_OPSET_VERSION},
                                                             {kMSDomain, 1}};
  //create a graph
  Model model("test", 
              false, 
              ModelMetaData(),
              ORT_TSTR(""),
              custom_op_registries_,
              domain_version_map,
              {},
              logger_);

  std::vector<onnxruntime::NodeArg*> input_args;
  std::vector<onnxruntime::NodeArg*> output_args;

  input_args.reserve(inputs.size());
  output_args.reserve(outputs.size());

  Graph& graph = model.MainGraph();
  std::unordered_map<std::string, OrtValue> initializer_map;
  size_t i = 0;

  for (auto input : inputs) {
    std::string name = "I" + std::to_string(i++);
    const Tensor& input_tensor = input.Get<Tensor>();
    ONNX_NAMESPACE::TypeProto input_tensor_type;
    input_tensor_type.mutable_tensor_type()->set_elem_type(input_tensor.GetElementType());
    auto& arg = graph.GetOrCreateNodeArg(name, &input_tensor_type);
    input_args.push_back(&arg);
    initializer_map[name] = input;
  }

  for (i = 0; i < outputs.size(); ++i) {
    auto& arg = graph.GetOrCreateNodeArg("O" + std::to_string(i), nullptr);
    output_args.push_back(&arg);
  }

  auto& node = graph.AddNode("node1", op_name, "eager mode node", input_args, output_args, attributes, domain);
  ORT_RETURN_IF_ERROR(graph.Resolve());

  node.SetExecutionProviderType(execution_provider_->Type());
  std::vector<const Node*> frame_nodes{&node};

  OptimizerExecutionFrame::Info info({&node}, initializer_map, graph.ModelPath(), *execution_provider_, [](std::string const& ) { return false; });
  const KernelCreateInfo* kernel_create_info = nullptr;
  ORT_RETURN_IF_ERROR(info.TryFindKernel(&node, &kernel_create_info));
  if (!kernel_create_info) {
    ORT_THROW("Could not find kernel name:", op_name, ", domain:", domain, ", version:", version);
  }
  // check whether the inputs are contiguous tensor
  const auto& may_strided_inputs = kernel_create_info->kernel_def->MayStridedInput();
  for (auto i = 0; i < inputs.size(); ++i) {
    const Tensor& input_tensor = inputs[i].Get<Tensor>();
    if (!input_tensor.IsContiguous() && std::find(may_strided_inputs.begin(), may_strided_inputs.end(),
                                                  static_cast<int>(i)) == may_strided_inputs.end())
      ORT_THROW("kernel name:", op_name, "'s ", i, "th input doesn't support non-contiguous tensor.");
  }

  auto kernel = info.CreateKernel(&node);
  if (!kernel) {
    ORT_THROW("Could not find kernel name:", op_name, ", domain:", domain, ", version:", version);
  }

  std::vector<int> fetch_mlvalue_idxs;
  for (const auto* node_out : node.OutputDefs()) {
    fetch_mlvalue_idxs.push_back(info.GetMLValueIndex(node_out->Name()));
  }

  OptimizerExecutionFrame frame(info, fetch_mlvalue_idxs, outputs);
  OpKernelContext op_kernel_context(&frame, kernel.get(), nullptr, logger_);
  ORT_RETURN_IF_ERROR(kernel->Compute(&op_kernel_context));

  return frame.GetOutputs(outputs);
}

}  // namespace onnxruntime
