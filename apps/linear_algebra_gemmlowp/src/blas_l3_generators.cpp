#include <vector>
#include "Halide.h"

using namespace Halide;

namespace {

// Generator class for BLAS gemm operations.
class GEMMGenerator :
        public Generator<GEMMGenerator> {
  public:
    typedef Generator<GEMMGenerator> Base;
    using Base::target;
    using Base::get_target;
    using Base::natural_vector_size;

    GeneratorParam<bool> transpose_A_ = {"transpose_A", false};
    GeneratorParam<bool> transpose_B_ = {"transpose_B", false};

    // Standard ordering of parameters in GEMM functions.
    Param<uint8_t>   a_ = {"a", 1};
    ImageParam A_in = {type_of<uint8_t>(), 2, "A_in"};
    ImageParam B_in = {type_of<uint8_t>(), 2, "B_in"};
    Param<uint8_t>   b_ = {"b", 1};
    ImageParam C_in = {type_of<uint8_t>(), 2, "C_in"};

    Var i, j, ii, ji, jii, iii, io, jo, ti, tj, t;

    Func build() {
        // Matrices are interpreted as column-major by default. The
        // transpose GeneratorParams are used to handle cases where
        // one or both is actually row major.
        const Expr num_rows = (A_in.width()/32)*32;
        const Expr num_cols = (B_in.height()/32)*32;
        const Expr sum_size = (A_in.height()/32)*32;

        const int vec = natural_vector_size(Int(32));
        const int s = vec * 2;

        // If they're both transposed, then reverse the order and transpose the result instead.
        bool transpose_AB = false;
        if ((bool)transpose_A_ && (bool)transpose_B_) {
            std::swap(A_, B_);
            transpose_A_.set(false);
            transpose_B_.set(false);
            transpose_AB = true;
        }

        Var ti[3], tj[3];
        Func result("result");

        // Swizzle A for better memory order in the inner loop.
        Func A("A"), B("B"), Btmp("Btmp"), As("As"), Atmp("Atmp");
        Atmp(i, j) = A_(i, j);

        if (transpose_A_) {
            As(i, j, io) = Atmp(j, io*s + i);
        } else {
            As(i, j, io) = Atmp(io*s + i, j);
        }

        A(i, j) = As(i % s, j, i / s);

        Btmp(i, j) = B_(i, j);
        if (transpose_B_) {
            B(i, j) = Btmp(j, i);
        } else {
            B(i, j) = Btmp(i, j);
        }

        Var k("k");
        Func prod;
        // Express all the products we need to do a matrix multiply as a 3D Func.
        prod(k, i, j) = cast<int32_t>(A(i, k) * B(k, j));

        // Reduce the products along k.
        Func AB("AB");
        RDom rv(0, sum_size);
        AB(i, j) += prod(rv, i, j);

        Func ABt("ABt");
        if (transpose_AB) {
            // Transpose A*B if necessary.
            ABt(i, j) = AB(j, i);
        } else {
            ABt(i, j) = AB(i, j);
        }

        // Do the part that makes it a 'general' matrix multiply.
        result(i, j) = cast<uint8_t>((a_ * ABt(i, j) + b_ * C_(i, j)));

        if (transpose_AB) {
            result
                .tile(i, j, ii, ji, 4, s).vectorize(ii).unroll(ji)
                .tile(i, j, ti[0], tj[0], i, j, s/4, 1);
        } else {
            result
                .tile(i, j, ii, ji, s, 4).vectorize(ii).unroll(ji)
                .tile(i, j, ti[0], tj[0], i, j, 1, s/4);
        }
        result.tile(ti[0], tj[0], ti[0], tj[0], ti[1], tj[1], 2, 2);

        // If we have enough work per task, parallelize over these tiles.
        result.specialize(num_rows >= 256 && num_cols >= 256)
            .fuse(tj[0], ti[0], t).parallel(t);

        // Otherwise tile one more time before parallelizing, or don't
        // parallelize at all.
        result.specialize(num_rows >= 128 && num_cols >= 128)
            .tile(ti[0], tj[0], ti[0], tj[0], ti[2], tj[2], 2, 2)
            .fuse(tj[0], ti[0], t).parallel(t);

        result.rename(tj[0], t);

        result.bound(i, 0, num_rows).bound(j, 0, num_cols);

        As.compute_root()
            .split(j, jo, ji, s).reorder(i, ji, io, jo)
            .unroll(i).vectorize(ji)
            .specialize(A_in.width() >= 256 && A_in.height() >= 256).parallel(jo, 4);

        Atmp.compute_at(As, io)
            .vectorize(i).unroll(j);

        if (transpose_B_) {
            B.compute_at(result, t)
                .tile(i, j, ii, ji, 8, 8)
                .vectorize(ii).unroll(ji);
            Btmp.reorder_storage(j, i)
                .compute_at(B, i)
                .vectorize(i)
                .unroll(j);
        }


        AB.compute_at(result, i)
            .unroll(j).vectorize(i)
            .update()
            .reorder(i, j, rv).unroll(j).unroll(rv, 2).vectorize(i);

        if (transpose_AB) {
            ABt.compute_at(result, i).unroll(i).vectorize(j);
        }

        A_in.set_min(0, 0).set_min(1, 0);
        B_in.set_bounds(0, 0, sum_size).set_min(1, 0);
        C_in.set_bounds(0, 0, num_rows).set_bounds(1, 0, num_cols);
        result.output_buffer().set_bounds(0, 0, num_rows).set_bounds(1, 0, num_cols);

        return result;
    }
};

RegisterGenerator<GEMMGenerator>    register_igemm("igemm");

}  // namespace
