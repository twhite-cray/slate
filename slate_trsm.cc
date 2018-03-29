//------------------------------------------------------------------------------
// Copyright (c) 2017, University of Tennessee
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the University of Tennessee nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL UNIVERSITY OF TENNESSEE BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------
// This research was supported by the Exascale Computing Project (17-SC-20-SC),
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//------------------------------------------------------------------------------
// Need assistance with the SLATE software? Join the "SLATE User" Google group
// by going to https://groups.google.com/a/icl.utk.edu/forum/#!forum/slate-user
// and clicking "Apply to join group". Upon acceptance, email your questions and
// comments to <slate-user@icl.utk.edu>.
//------------------------------------------------------------------------------

#include "slate.hh"
#include "slate_Debug.hh"
#include "slate_Matrix.hh"
#include "slate_internal.hh"

namespace slate {

// specialization namespace differentiates, e.g.,
// internal::trsm from internal::specialization::trsm
namespace internal {
namespace specialization {

///-----------------------------------------------------------------------------
/// \brief
/// Distributed parallel triangular matrix multiplication.
/// Generic implementation for any target.
// Note A and B are passed by value, so we can transpose if needed
// (for side = right) without affecting caller.
template <Target target, typename scalar_t>
void trsm(slate::internal::TargetType<target>,
          Side side, Diag diag,
          scalar_t alpha, TriangularMatrix<scalar_t> A,
                                    Matrix<scalar_t> B,
          int64_t lookahead)
{
    using namespace blas;

    // if on right, change to left by (conj)-transposing A and B to get
    // op(B) = op(A)^{-1} * op(B)
    if (side == Side::Right) {
        if (A.op() == Op::ConjTrans || B.op() == Op::ConjTrans) {
            A = conj_transpose(A);
            B = conj_transpose(B);
            alpha = conj(alpha);
        }
        else {
            A = transpose(A);
            B = transpose(B);
        }
    }

    // B is mt-by-nt, A is mt-by-mt (assuming side = left)
    assert(A.mt() == B.mt());
    assert(A.nt() == B.mt());

    int64_t mt = B.mt();
    int64_t nt = B.nt();

    if (target == Target::Devices) {
        B.allocateBatchArrays();
        B.reserveDeviceWorkspace();
    }

    // OpenMP needs pointer types, but vectors are exception safe
    std::vector< uint8_t > row_vector(A.nt());
    uint8_t *row = row_vector.data();

    #pragma omp parallel
    #pragma omp master
    {
        if (A.uplo_logical() == Uplo::Lower) {
            // ----------------------------------------
            // Lower/NoTrans or Upper/Trans, Left case
            // Forward sweep
            for (int64_t k = 0; k < mt; ++k) {
                scalar_t alph = k == 0 ? alpha : scalar_t(1.0);

                // panel (Akk tile)
                #pragma omp task depend(inout:row[k]) priority(1)
                {
                    // send A(k, k) to ranks owning block row B(k, :)
                    A.template tileBcast<target>(
                        k, k, B.sub(k, k, 0, nt-1));

                    // solve A(k, k) B(k, :) = alpha B(k, :)
                    internal::trsm<Target::HostTask>(
                        Side::Left, diag,
                        alph, A.sub(k, k),
                              B.sub(k, k, 0, nt-1), 1);

                    // send A(i=k+1:mt-1, k) to ranks owning block row B(i, :)
                    for (int64_t i = k+1; i < mt; ++i)
                        A.template tileBcast(
                            i, k, B.sub(i, i, 0, nt-1));

                    // send B(k, j=0:nt-1) to ranks owning block col B(k+1:mt-1, j)
                    for (int64_t j = 0; j < nt; ++j)
                        B.template tileBcast(
                            k, j, B.sub(k+1, mt-1, j, j));
                }

                // lookahead update, B(k+1:k+la, :) -= A(k+1:k+la, k) B(k, :)
                for (int64_t i = k+1; i < k+1+lookahead && i < mt; ++i) {
                    #pragma omp task depend(in:row[k]) \
                                     depend(inout:row[i]) priority(1)
                    {
                        internal::gemm<Target::HostTask>(
                            scalar_t(-1.0), A.sub(i, i, k, k),
                                            B.sub(k, k, 0, nt-1),
                            alph,           B.sub(i, i, 0, nt-1), 1);
                    }
                }

                // trailing update, B(k+1+la:mt-1, :) -= A(k+1+la:mt-1, k) B(k, :)
                // Updates rows k+1+la to mt-1, but two depends are sufficient:
                // depend on k+1+la is all that is needed in next iteration;
                // depend on mt-1 daisy chains all the trailing updates.
                if (k+1+lookahead < mt) {
                    #pragma omp task depend(in:row[k]) \
                                     depend(inout:row[k+1+lookahead]) \
                                     depend(inout:row[mt-1])
                    {
                        internal::gemm<target>(
                            scalar_t(-1.0), A.sub(k+1+lookahead, mt-1, k, k),
                                            B.sub(k, k, 0, nt-1),
                            alph,           B.sub(k+1+lookahead, mt-1, 0, nt-1));
                    }
                }
            }
        }
        else {
            // ----------------------------------------
            // Upper/NoTrans or Lower/Trans, Left case
            // Backward sweep
            for (int64_t k = mt-1; k >= 0; --k) {
                scalar_t alph = k == (mt-1) ? alpha : scalar_t(1.0);

                // panel (Akk tile)
                #pragma omp task depend(inout:row[k]) priority(1)
                {
                    // send A(k, k) to ranks owning block row B(k, :)
                    A.template tileBcast<target>(
                        k, k, B.sub(k, k, 0, nt-1));

                    // solve A(k, k) B(k, :) = alpha B(k, :)
                    internal::trsm<Target::HostTask>(
                        Side::Left, diag,
                        alph, A.sub(k, k),
                              B.sub(k, k, 0, nt-1), 1);

                    // send A(i=0:k-1, k) to ranks owning block row B(i, :)
                    for (int64_t i = 0; i < k; ++i)
                        A.template tileBcast(
                            i, k, B.sub(i, i, 0, nt-1));

                    // send B(k, j=0:nt-1) to ranks owning block col B(0:k-1, j)
                    for (int64_t j = 0; j < nt; ++j)
                        B.template tileBcast(
                            k, j, B.sub(0, k-1, j, j));
                }

                // lookahead update, B(k-la:k-1, :) -= A(k-la:k-1, k) B(k, :)
                for (int64_t i = k-1; i > k-1-lookahead && i >= 0; --i) {
                    #pragma omp task depend(in:row[k]) \
                                     depend(inout:row[i]) priority(1)
                    {
                        internal::gemm<Target::HostTask>(
                            scalar_t(-1.0), A.sub(i, i, k, k),
                                            B.sub(k, k, 0, nt-1),
                            alph,           B.sub(i, i, 0, nt-1), 1);
                    }
                }

                // trailing update, B(0:k-1-la, :) -= A(0:k-1-la, k) B(k, :)
                // Updates rows 0 to k-1-la, but two depends are sufficient:
                // depend on k-1-la is all that is needed in next iteration;
                // depend on 0 daisy chains all the trailing updates.
                if (k-1-lookahead >= 0) {
                    #pragma omp task depend(in:row[k]) \
                                     depend(inout:row[k-1-lookahead]) \
                                     depend(inout:row[0])
                    {
                        internal::gemm<target>(
                            scalar_t(-1.0), A.sub(0, k-1-lookahead, k, k),
                                            B.sub(k, k, 0, nt-1),
                            alph,           B.sub(0, k-1-lookahead, 0, nt-1));
                    }
                }
            }
        }
    }

    // todo: we need a function that updates origins that are not valid
    for (int64_t i = 0; i < B.mt(); ++i)
        for (int64_t j = 0; j < B.nt(); ++j)
            if (B.tileIsLocal(i, j))
                B.tileMoveToHost(i, j, B.tileDevice(i, j));

    B.clearWorkspace();
}

} // namespace specialization
} // namespace internal

///-----------------------------------------------------------------------------
/// \brief
///
/// Precision and target templated function.
template <Target target, typename scalar_t>
void trsm(blas::Side side, blas::Diag diag,
          scalar_t alpha, TriangularMatrix<scalar_t>& A,
                                    Matrix<scalar_t>& B,
          const std::map<Option, Value>& opts)
{
    int64_t lookahead;
    try {
        lookahead = opts.at(Option::Lookahead).i_;
        assert(lookahead >= 0);
    }
    catch (std::out_of_range) {
        lookahead = 1;
    }

    internal::specialization::trsm(internal::TargetType<target>(),
                                   side, diag,
                                   alpha, A,
                                          B,
                                   lookahead);
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void trsm< Target::HostTask, float >(
    blas::Side side, blas::Diag diag,
    float alpha, TriangularMatrix<float>& A,
                           Matrix<float>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostNest, float >(
    blas::Side side, blas::Diag diag,
    float alpha, TriangularMatrix<float>& A,
                           Matrix<float>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostBatch, float >(
    blas::Side side, blas::Diag diag,
    float alpha, TriangularMatrix<float>& A,
                           Matrix<float>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::Devices, float >(
    blas::Side side, blas::Diag diag,
    float alpha, TriangularMatrix<float>& A,
                           Matrix<float>& B,
    const std::map<Option, Value>& opts);

// ----------------------------------------
template
void trsm< Target::HostTask, double >(
    blas::Side side, blas::Diag diag,
    double alpha, TriangularMatrix<double>& A,
                            Matrix<double>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostNest, double >(
    blas::Side side, blas::Diag diag,
    double alpha, TriangularMatrix<double>& A,
                            Matrix<double>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostBatch, double >(
    blas::Side side, blas::Diag diag,
    double alpha, TriangularMatrix<double>& A,
                            Matrix<double>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::Devices, double >(
    blas::Side side, blas::Diag diag,
    double alpha, TriangularMatrix<double>& A,
                            Matrix<double>& B,
    const std::map<Option, Value>& opts);

// ----------------------------------------
template
void trsm< Target::HostTask, std::complex<float> >(
    blas::Side side, blas::Diag diag,
    std::complex<float> alpha, TriangularMatrix<std::complex<float>>& A,
                                         Matrix<std::complex<float>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostNest, std::complex<float> >(
    blas::Side side, blas::Diag diag,
    std::complex<float> alpha, TriangularMatrix<std::complex<float>>& A,
                                         Matrix<std::complex<float>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostBatch, std::complex<float> >(
    blas::Side side, blas::Diag diag,
    std::complex<float> alpha, TriangularMatrix<std::complex<float>>& A,
                                         Matrix<std::complex<float>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::Devices, std::complex<float> >(
    blas::Side side, blas::Diag diag,
    std::complex<float> alpha, TriangularMatrix<std::complex<float>>& A,
                                         Matrix<std::complex<float>>& B,
    const std::map<Option, Value>& opts);

// ----------------------------------------
template
void trsm< Target::HostTask, std::complex<double> >(
    blas::Side side, blas::Diag diag,
    std::complex<double> alpha, TriangularMatrix<std::complex<double>>& A,
                                          Matrix<std::complex<double>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostNest, std::complex<double> >(
    blas::Side side, blas::Diag diag,
    std::complex<double> alpha, TriangularMatrix<std::complex<double>>& A,
                                          Matrix<std::complex<double>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::HostBatch, std::complex<double> >(
    blas::Side side, blas::Diag diag,
    std::complex<double> alpha, TriangularMatrix<std::complex<double>>& A,
                                          Matrix<std::complex<double>>& B,
    const std::map<Option, Value>& opts);

template
void trsm< Target::Devices, std::complex<double> >(
    blas::Side side, blas::Diag diag,
    std::complex<double> alpha, TriangularMatrix<std::complex<double>>& A,
                                          Matrix<std::complex<double>>& B,
    const std::map<Option, Value>& opts);

} // namespace slate
