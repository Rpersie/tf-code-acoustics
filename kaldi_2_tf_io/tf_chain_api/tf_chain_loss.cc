
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/shape_inference.h"

#include <sys/time.h>

#include "tf-2-kaldi-api.h"

using namespace tensorflow;
using tensorflow::shape_inference::DimensionHandle;
using tensorflow::shape_inference::InferenceContext;
using tensorflow::shape_inference::ShapeHandle;

REGISTER_OP("ChainLoss")
	.Input("inputs: float")
	.Input("indexs: int32")
	.Input("in_labels: int32")
	.Input("weights: float")
	.Input("statesinfo: int32")
	.Input("num_states: int32")
	.Input("supervision_weights: float")
	.Input("num_sequences: int32")
	.Input("frames_per_sequence: int32")
	.Input("label_dim: int32")
	.Attr("den_indexs: tensor = { dtype: DT_INT32 }")
	.Attr("den_in_labels: tensor = { dtype: DT_INT32 }")
	.Attr("den_weights: tensor = { dtype: DT_INT32 }")
	.Attr("den_statesinfo: tensor = { dtype: DT_INT32 }")
	.Attr("den_num_states: int32 = 0")
	.Attr("l2_regularize: float = 0.0")
	.Attr("leaky_hmm_coefficient: float = 0.0")
	.Attr("xent_regularize: float = 0.0")
	.Output("objf: float")
	.Output("gradient: float")
	.SetShapeFn([](InferenceContext* c)
	{
		ShapeHandle inputs;         // nnet forward output
		ShapeHandle indexs;         // next inputs it's lattice info
		ShapeHandle pdf_values;
		ShapeHandle lm_ws;
		ShapeHandle am_ws;
		ShapeHandle statesinfo;
		ShapeHandle num_states;

		// check shape
		TF_RETURN_IF_ERROR(c->WithRank(c->input(0), 3, &inputs));
		TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 3, &indexs));
		TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 2, &in_labels));
		TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 2, &weights));
		TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 3, &statesinfo));
		TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 1, &num_states));

		// Get batch size from inputs and sequence_length, and update inputs
		// with the merged batch_size since it is returned.
		DimensionHandle batch_size;		
		TF_RETURN_IF_ERROR(
				c->Merge(c->Dim(inputs, 1), c->Dim(sequence_length, 0), &batch_size));

		TF_RETURN_IF_ERROR(c->ReplaceDim(inputs, 1, batch_size, &inputs));

		c->set_output(0, c->Vector(batch_size));
		c->set_output(1, inputs);

		return Status::OK();
	});


namespace chain_loss
{
class ChainLossOp: public OpKernel
{
public:
	explicit ChainLossOp(OpKernelConstruction* ctx) : OpKernel(ctx)
	{
		// den fst data
		OP_REQUIRES_OK(ctx, ctx->GetAttr("den_indexs", &_den_indexs));
		OP_REQUIRES(ctx, _den_indexs.dtype() == tf::DT_INT32,
				errors::InvalidArgument("_den_indexs must be int32, got ",
					DataTypeString(_den_indexs.dtype())));

		OP_REQUIRES_OK(ctx, ctx->GetAttr("den_in_labels", &_den_in_labels));
		OP_REQUIRES(ctx, _den_in_labels.dtype() == tf::DT_INT32,
				errors::InvalidArgument("_den_in_labels must be int32, got ",
					DataTypeString(_den_in_labels.dtype())));

		OP_REQUIRES_OK(ctx, ctx->GetAttr("den_weights", &_den_weights));
		OP_REQUIRES(ctx, _den_weights.dtype() == tf::DT_FLOAT,
				errors::InvalidArgument("_den_weights must be float, got ",
					DataTypeString(_den_weights.dtype())));

		OP_REQUIRES_OK(ctx, ctx->GetAttr("den_statesinfo", &_den_statesinfo));
		OP_REQUIRES(ctx, _den_statesinfo.dtype() == tf::DT_INT32,
				errors::InvalidArgument("_den_statesinfo must be , got ",
					DataTypeString(_den_statesinfo.dtype())));

		OP_REQUIRES_OK(ctx, ctx->GetAttr("den_num_states", &_den_num_states));

		// loss config
		OP_REQUIRES_OK(ctx, ctx->GetAttr("l2_regularize", &_l2_regularize));
		OP_REQUIRES_OK(ctx, ctx->GetAttr("leaky_hmm_coefficient", &_leaky_hmm_coefficient));
		OP_REQUIRES_OK(ctx, ctx->GetAttr("xent_regularize", &_xent_regularize));
	}

	void Compute(OpKernelContext* ctx) override
	{
#ifdef DEBUG_SPEED
		struct timeval start;
		struct timeval end;
		gettimeofday(&start, NULL);
#endif
		const Tensor* inputs;
		const Tensor* indexs;
		const Tensor* pdf_values;
		const Tensor* lm_ws;
		const Tensor* am_ws;
		const Tensor* statesinfo;
		const Tensor* num_states;

		OP_REQUIRES_OK(ctx, ctx->input("inputs", &inputs));
		OP_REQUIRES_OK(ctx, ctx->input("indexs", &indexs));
		OP_REQUIRES_OK(ctx, ctx->input("pdf_values", &pdf_values));
		OP_REQUIRES_OK(ctx, ctx->input("lm_ws", &lm_ws));
		OP_REQUIRES_OK(ctx, ctx->input("am_ws", &am_ws));
		OP_REQUIRES_OK(ctx, ctx->input("statesinfo", &statesinfo));
		OP_REQUIRES_OK(ctx, ctx->input("num_states", &num_states));

		OP_REQUIRES(ctx, inputs->shape().dims() == 3,
				errors::InvalidArgument("inputs is not a 3-Tensor"));

		OP_REQUIRES(ctx, indexs->shape().dims() == 3,
				errors::InvalidArgument("indexs is not a 3-Tensor"));

		OP_REQUIRES(ctx, TensorShapeUtils::IsMatrix(pdf_values->shape()),
				errors::InvalidArgument("pdf_values is not a matrix"));

		OP_REQUIRES(ctx, TensorShapeUtils::IsMatrix(lm_ws->shape()),
				errors::InvalidArgument("lm_ws is not a matrix"));

		OP_REQUIRES(ctx, TensorShapeUtils::IsMatrix(am_ws->shape()),
				errors::InvalidArgument("am_ws is not a matrix"));
		OP_REQUIRES(ctx, statesinfo->shape().dims() == 3,
				errors::InvalidArgument("statesinfo is not 3-Tensor"));

		const TensorShape& inputs_shape = inputs->shape();
		const int64 max_time = inputs_shape.dim_size(0);
		const int64 batch_size = inputs_shape.dim_size(1);
		const int64 num_classes_raw = inputs_shape.dim_size(2);

		const TensorShape& indexs_shape = indexs->shape();
		const int32 max_num_arcs = indexs_shape.dim_size(1);

		const TensorShape& statesinfo_shape = statesinfo->shape();
		const int32 max_num_states = statesinfo_shape.dim_size(1);

		// check num_classes_raw less then std::numeric_limits<int>::max()
		OP_REQUIRES(
				ctx, FastBoundsCheck(num_classes_raw, std::numeric_limits<int>::max()),
				errors::InvalidArgument("num_classes cannot exceed max int"));
		const int num_classes = static_cast<const int>(num_classes_raw);

		// malloc loss space
		//Tensor* loss = nullptr;
		//OP_REQUIRES_OK(ctx, ctx->allocate_output("loss", sequence_length->shape(), &loss));
		//auto loss_t = loss->vec<float>();

		// malloc gradient space
		Tensor* gradient;
		OP_REQUIRES_OK(ctx,
				ctx->allocate_output("gradient", inputs_shape, &gradient));


		auto inputs_t = inputs->tensor<float, 3>();
		auto gradient_t = gradient->tensor<float, 3>();

		// gradient set zero
		// the setZero is so slow,so I decide zet zero in hubo::MMILoss
		// gradient_t.setZero();

		auto indexs_t = indexs->tensor<int, 3>();
		auto pdf_values_t = pdf_values->matrix<int>();
		auto lm_ws_t = lm_ws->matrix<float>();
		auto am_ws_t = am_ws->matrix<float>();
		auto statesinfo_t = statesinfo->tensor<int, 3>();
		auto num_states_t = num_states->vec<int>();
		auto labels_t = labels->matrix<int>();

#ifdef DEBUG_SPEED
		gettimeofday(&end, NULL);
		std::cout << "DEBUG_SPEED : " << __FILE__ " : mmi_loss_op process data time:"
			<< (end.tv_sec - start.tv_sec)+(end.tv_usec-start.tv_usec)*1.0/1e6<< std::endl;
#endif

		bool ret_chain = hubo::ChainLoss();

#ifdef DEBUG_SPEED
		gettimeofday(&end, NULL);
		std::cout << "DEBUG_SPEED : " << __FILE__ << " : mmi_loss_op calculate mmi time:"
			<< (end.tv_sec - start.tv_sec)+(end.tv_usec-start.tv_usec)*1.0/1e6<< std::endl;
#endif
	}
private:
	Tensor _den_indexs;
	Tensor _den_in_labels;
	Tensor _den_weights;
	Tensor _den_statesinfo;
	int	_den_num_states;
	// l2 regularization constant on the 'chain' output; the actual term added to
	// the objf will be -0.5 times this constant times the squared l2 norm.
	// (squared so it's additive across the dimensions).  e.g. try 0.0005.
	float _l2_regularize;
	
	// Coefficient for 'leaky hmm'.  This means we have an epsilon-transition from
	// each state to a special state with probability one, and then another
	// epsilon-transition from that special state to each state, with probability
	// leaky_hmm_coefficient times [initial-prob of destination state].  Imagine
	// we make two copies of each state prior to doing this, version A and version
	// B, with transition from A to B, so we don't have to consider epsilon loops-
	// or just imagine the coefficient is small enough that we can ignore the
	// epsilon loops.
	float _leaky_hmm_coefficient;
	
	// Cross-entropy regularization constant.  (e.g. try 0.1).  If nonzero,
	// the network is expected to have an output named 'output-xent', which
	// should have a softmax as its final nonlinearity.
	float _xent_regularize;
	TF_DISALLOW_COPY_AND_ASSIGN(ChainLossOp);
};

REGISTER_KERNEL_BUILDER(Name("ChainLoss").Device(DEVICE_CPU), ChainLossOp);

} // namespace


