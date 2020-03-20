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
// For assistance with SLATE, email <slate-user@icl.utk.edu>.
// You can also join the "SLATE User" Google group by going to
// https://groups.google.com/a/icl.utk.edu/forum/#!forum/slate-user,
// signing in with your Google credentials, and then clicking "Join group".
//------------------------------------------------------------------------------

#include "slate/slate.hh"
#include "aux/Debug.hh"
#include "slate/HermitianMatrix.hh"
#include "slate/Tile_blas.hh"
#include "slate/HermitianBandMatrix.hh"
#include "internal/internal.hh"

namespace slate {

//------------------------------------------------------------------------------
template <typename scalar_t>
void hegv( int type,
           lapack::Job jobz,
           HermitianMatrix<scalar_t> A,
           HermitianMatrix<scalar_t> B,           
           std::vector< blas::real_type<scalar_t> >& W,
           Matrix<scalar_t>& V,
           const std::map<Option, Value>& opts)
{
    using real_t = blas::real_type<scalar_t>;

    // if upper, change to lower
    if (A.uplo() == Uplo::Upper) {
        A = conj_transpose(A);
    }
    if (B.uplo() == Uplo::Upper) {
        B = conj_transpose(B);
    }

    scalar_t one = 1.;

    // 1. Form a Cholesky factorization of B. 
    potrf(B, opts);

    // 2. Transform problem to standard eigenvalue problem.
    // hegst( type, A, B);
    // A will be overwritten by Ahat based on the type as follows:
    // if (type == 1) {
        // Ahat = inv(L) * A * inv(conj_transpose(L));
    // }
    // else {
        // Ahat = conj_transpose(L) * A * L;
    // }

    // 3. Solve the standard eigenvalue problem and solve.
    // heev(Ahat, W, V, opts); 

    // 4. Backtransform eigenvectors to the original problem.
    auto L = slate::TriangularMatrix<scalar_t>( 
               slate::Uplo::Lower, slate::Diag::NonUnit, B );  
    if (type == 1 || type == 2) {
        // x = inv(L)**T*y
        slate::trsm(slate::Side::Left, one, L, V, opts);
    }
    else {
        // x = L*y
        slate::trmm(slate::Side::Left, one, L, V, opts);
    }
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void hegv<float>(
     int type,
     lapack::Job jobz,
     HermitianMatrix<float>& A,
     HermitianMatrix<float>& B,
     std::vector<float>& W,
    Matrix<float>& V,
     const std::map<Option, Value>& opts);

template
void hegv<double>(
     int type,
     lapack::Job jobz,
     HermitianMatrix<double>& A,
     HermitianMatrix<double>& B,
     std::vector<double>& W,
    Matrix<double>& V,
     const std::map<Option, Value>& opts);

template
void hegv< std::complex<float> >(
     int type,
     lapack::Job jobz,
     HermitianMatrix< std::complex<float> >& A,
     HermitianMatrix< std::complex<float> >& B,
     std::vector<float>& W,
    Matrix< std::complex<float> >& V,
     const std::map<Option, Value>& opts);

template
void hegv< std::complex<double> >(
     int type,
     lapack::Job jobz,
     HermitianMatrix< std::complex<double> >& A,
     HermitianMatrix< std::complex<double> >& B,
     std::vector<double>& W,
    Matrix< std::complex<double> >& V,
     const std::map<Option, Value>& opts);

} // namespace slate
