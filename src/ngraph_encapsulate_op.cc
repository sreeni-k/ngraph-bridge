/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"

#include "ngraph_builder.h"
#include "ngraph_cluster_manager.h"
#include "ngraph_utils.h"

namespace tf = tensorflow;

namespace ngraph_bridge {
extern const char* const DEVICE_NGRAPH_CPU;

REGISTER_OP("NGraphEncapsulate")
    .Input("args: Targuments")
    .Attr("Targuments: list(type) >= 0")
    .Output("results: Tresults")
    .Attr("Tresults: list(type) >= 0")
    .Attr("ngraph_cluster: int")
    .SetIsStateful()
    .Doc("nGraph Encapsulation Op. For use by the nGraph JIT only.");

class NGraphEncapsulateOp : public tf::OpKernel {
 public:
  explicit NGraphEncapsulateOp(tf::OpKernelConstruction* ctx)
      : tf::OpKernel(ctx), m_graph(tf::OpRegistry::Global()) {
    int ngraph_cluster;
    tf::GraphDef* graph_def;

    // TODO(amprocte): need to check status result here.
    OP_REQUIRES_OK(ctx, ctx->GetAttr<int>("ngraph_cluster", &ngraph_cluster));
    graph_def = NGraphClusterManager::GetClusterGraph(ngraph_cluster);

    tf::GraphConstructorOptions opts;
    opts.allow_internal_ops = true;
    // TODO(amprocte): need to check status result here.
    OP_REQUIRES_OK(ctx, tf::ConvertGraphDefToGraph(opts, *graph_def, &m_graph));
  }

  ~NGraphEncapsulateOp() override {
    // d-tor
  }

  void Compute(tf::OpKernelContext* ctx) override {
    // Get the inputs
    std::vector<tf::TensorShape> input_shapes;
    for (int i = 0; i < ctx->num_inputs(); i++) {
      const tf::Tensor& input_tensor = ctx->input(i);
      input_shapes.push_back(input_tensor.shape());
    }

    // Compile the graph using nGraph. (TODO(amprocte): need a compilation
    // cache.
    // shared_ptr<ng::Function> ng_function;
    if (ng_function == nullptr) {
      OP_REQUIRES_OK(
          ctx, Builder::TranslateGraph(input_shapes, &m_graph, ng_function));
    }

    // Create the nGraph backend (TODO(amprocte): should probably put this
    // into the resource manager rather than re-create each time, though I
    // don't know how expensive this actually is.)
    auto backend = ng::runtime::Backend::create("CPU");

    // Allocate tensors for arguments.
    vector<shared_ptr<ng::runtime::TensorView>> ng_inputs;
    for (int i = 0; i < input_shapes.size(); i++) {
      ng::Shape ng_shape(input_shapes[i].dims());
      for (int j = 0; j < input_shapes[i].dims(); ++j) {
        ng_shape[j] = input_shapes[i].dim_size(j);
      }

      ng::element::Type ng_element_type;
      OP_REQUIRES_OK(ctx, TFDataTypeToNGraphElementType(ctx->input(i).dtype(),
                                                        &ng_element_type));

      void* src_ptr = (void*)tf::DMAHelper::base(&ctx->input(i));
      auto t = backend->create_tensor(ng_element_type, ng_shape, src_ptr);
      ng_inputs.push_back(t);
    }

    // Allocate tensors for the results.
    vector<shared_ptr<ng::runtime::TensorView>> outputs;
    for (auto i = 0; i < ng_function->get_output_size(); i++) {
      auto shape = ng_function->get_output_shape(i);
      auto elem_type = ng_function->get_output_element_type(i);

      // Create the TF output tensor
      vector<tf::int64> dims;
      for (auto dim : shape) {
        dims.push_back(dim);
      }
      tf::TensorShape tf_shape(dims);
      tf::Tensor* output_tensor = nullptr;
      OP_REQUIRES_OK(ctx, ctx->allocate_output(i, tf_shape, &output_tensor));

      // Make sure the nGraph-inferred element type agrees with what TensorFlow
      // expected.
      ng::element::Type expected_elem_type;
      OP_REQUIRES_OK(
          ctx, TFDataTypeToNGraphElementType(ctx->expected_output_dtype(i),
                                             &expected_elem_type));
      OP_REQUIRES(
          ctx, elem_type == expected_elem_type,
          tf::errors::Internal("Element type inferred by nGraph does not match "
                               "the element type expected by TensorFlow"));

      // Create the nGraph output tensor
      void* dst_ptr = tf::DMAHelper::base(output_tensor);
      auto t_result = backend->create_tensor(elem_type, shape, dst_ptr);

      outputs.push_back(t_result);
    }

    // Execute the nGraph function.
    backend->call(ng_function, outputs, ng_inputs);
  }

 private:
  tf::Graph m_graph;
  std::shared_ptr<ngraph::Function> ng_function;
};

}  // namespace ngraph_bridge

namespace tensorflow {
REGISTER_KERNEL_BUILDER(
    Name("NGraphEncapsulate").Device(ngraph_bridge::DEVICE_NGRAPH_CPU),
    ngraph_bridge::NGraphEncapsulateOp);
}
