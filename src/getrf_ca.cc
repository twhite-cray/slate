// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "aux/Debug.hh"
#include "slate/Matrix.hh"
#include "slate/Tile_blas.hh"
#include "slate/TriangularMatrix.hh"
#include "internal/internal.hh"

namespace slate {

// specialization namespace differentiates, e.g.,
// internal::getrf_ca from internal::specialization::getrf
namespace internal {
namespace specialization {

//------------------------------------------------------------------------------
/// Distributed parallel LU factorization.
/// Generic implementation for any target.
/// Panel and lookahead computed on host using Host OpenMP task.
/// @ingroup gesv_specialization
///
template <Target target, typename scalar_t>
void getrf_ca(slate::internal::TargetType<target>,
           Matrix<scalar_t>& A, Pivots& pivots,
           int64_t ib, int max_panel_threads, int64_t lookahead)
{
    // using real_t = blas::real_type<scalar_t>;
    using BcastList = typename Matrix<scalar_t>::BcastList;
    using BcastListTag = typename Matrix<scalar_t>::BcastListTag;

    // Host can use Col/RowMajor for row swapping,
    // RowMajor is slightly more efficient.
    // Layout host_layout = Layout::RowMajor;
    // Layout target_layout = Layout::RowMajor;
    // todo: RowMajor causes issues with tileLayoutReset() when A origin is
    //       ScaLAPACK
    Layout host_layout = Layout::ColMajor;
    Layout target_layout = Layout::ColMajor;
    // GPU Devices use RowMajor for efficient row swapping.
    if (target == Target::Devices)
        target_layout = Layout::RowMajor;

    if (target == Target::Devices) {
        A.allocateBatchArrays();
        A.reserveDeviceWorkspace();
    }

    const int priority_one = 1;
    const int priority_zero = 0;
    int64_t A_nt = A.nt();
    int64_t A_mt = A.mt();
    int64_t min_mt_nt = std::min(A.mt(), A.nt());
    int life_factor_one = 1;
    bool is_shared = lookahead > 0; //TODO::RABAB
    pivots.resize(min_mt_nt);

    // OpenMP needs pointer types, but vectors are exception safe
    std::vector< uint8_t > column_vector(A_nt);
    std::vector< uint8_t > diag_vector(A_nt);
    uint8_t* column = column_vector.data();
    uint8_t* diag = diag_vector.data();
    // Running two listBcastMT's simultaneously can hang due to task ordering
    // This dependency avoids that
    uint8_t listBcastMT_token;
    SLATE_UNUSED(listBcastMT_token); // Only used by OpenMP


    #if 0
     for (int i=0; i<A.mt(); i++){
         if (A.tileIsLocal(i, 0)){
             if( A.mpiRank() == 0){
                 for(int m=0; m<A.tileMb(i);m++){
                     for(int n=0; n<A.tileMb(i);n++){
                         std::cout<<A(i,0)(m,n)<<",";
                     }
                     std::cout<<";";
                 }
             }
         }
     }
    #endif
   // workspace
    auto Awork = A.emptyLike();

  //  #pragma omp parallel
  //  #pragma omp master
    {
        omp_set_nested(1);
        for (int64_t k = 0; k < min_mt_nt; ++k) {

            int64_t diag_len = std::min(A.tileMb(k), A.tileNb(k));
            pivots.at(k).resize(diag_len);

            auto Apanel = Awork.sub( k, A_mt-1, k, k );
            Apanel.insertLocalTiles();

            // panel, high priority
         //   #pragma omp task depend(inout:column[k]) \
                             depend(out:diag[k]) \
                             priority(priority_one)
            {
                // factor A(k:mt-1, k)
                internal::getrf_ca<Target::HostTask>(
                    A.sub(k, A_mt-1, k, k),  std::move(Apanel), diag_len, ib,
                    pivots.at(k), max_panel_threads, priority_one);

                // Root broadcasts the pivot to all ranks.
                // todo: Panel ranks send the pivots to the right.
                {
                    trace::Block trace_block("MPI_Bcast");

                    MPI_Bcast(pivots.at(k).data(),
                              sizeof(Pivot)*pivots.at(k).size(),
                              MPI_BYTE, A.tileRank(k, k), A.mpiComm());
                }

           // swap rows in A(k+1:A_mt-1, k)
           int tag_kl1 = k+1;
           internal::permuteRows<Target::HostTask>(
                   Direction::Forward, A.sub(k, A_mt-1, k, k),
                   pivots.at(k), host_layout, priority_one, tag_kl1, 0);

           #if 0
            for (int i=0; i<A.mt(); i++){
                if (A.tileIsLocal(2,2 )){
                    if( k==2){
                        std::cout<<"\n Factore Tile: "<<i<<" of rank: "<<A.mpiRank()<<std::endl;
                        for(int m=0; m<A.tileMb(i);m++){
                             for(int n=0; n<A.tileMb(i);n++){
                                 std::cout<<A(2,2)(m,n)<<",";
                             }
                             std::cout<<std::endl;
                        }
                    }
                }
            }
           #endif

           internal::copy<Target::HostTask>( Apanel.sub( 0, 0, 0, 0 ), A.sub( k, k, k, k ));


           //Update panel
           int tag_k = k;
           BcastList bcast_list_A;
           bcast_list_A.push_back({k, k, {A.sub(k+1, A_mt-1, k, k),
                                          A.sub(k, k, k+1, A_nt-1)}});
           A.template listBcast<target>(
                bcast_list_A, host_layout, tag_k, life_factor_one, true);

           Apanel.clear();
           }

           // #pragma omp task depend(inout:column[k]) \
                            depend(in:diag[k]) \
                            depend(inout:listBcastMT_token) \
                            priority(priority_one)
           {
               // swap rows in A(k+1:A_mt-1, k)
               /*int tag_kl1 = k+1;
               internal::permuteRows<target>(
                       Direction::Forward, A.sub(k, A_mt-1, k, k),
                       pivots.at(k), target_layout, priority_one, tag_kl1, 0);*/

               auto Akk = A.sub(k, k, k, k);
               auto Tkk = TriangularMatrix<scalar_t>(Uplo::Upper, Diag::NonUnit, Akk);

               internal::trsm<target>(
                       Side::Right,
                       scalar_t(1.0), std::move(Tkk),
                       A.sub(k+1, A_mt-1, k, k),
                       priority_one, Layout::ColMajor, 0);

               BcastListTag bcast_list;
               // bcast the tiles of the panel to the right hand side
               for (int64_t i = k+1; i < A_mt; ++i) {
                   // send A(i, k) across row A(i, k+1:nt-1)
                   const int64_t tag = i;
                   bcast_list.push_back({i, k, {A.sub(i, i, k+1, A_nt-1)}, tag});
               }
               A.template listBcastMT<target>(
                 bcast_list, Layout::ColMajor, life_factor_one, is_shared);

           #if 0
               for (int i=0; i<A.mt(); i++){
                   if (A.tileIsLocal(i, 0)){
                       if( A.mpiRank() == 0){
                           std::cout<<"\n Factore Tile: "<<i<<" of rank: "<<A.mpiRank()<<std::endl;
                           for(int m=0; m<A.tileMb(i);m++){
                               for(int n=0; n<A.tileMb(i);n++){
                                   std::cout<<A(i,0)(m,n)<<",";
                               }
                               std::cout<<std::endl;
                           }
                       }
                   }
               }
            #endif
           }

            // update lookahead column(s), high priority
            for (int64_t j = k+1; j < k+1+lookahead && j < A_nt; ++j) {
            ///    #pragma omp task depend(in:diag[k]) \
                                 depend(inout:column[j]) \
                                 priority(priority_one)
                {
                    int tag_j = j;
                    internal::permuteRows<target>(
                            Direction::Forward, A.sub(k, A_mt-1, j, j), pivots.at(k),
                            target_layout, priority_one, tag_j, j-k+1);

                    auto Akk = A.sub(k, k, k, k);
                    auto Tkk =
                        TriangularMatrix<scalar_t>(Uplo::Lower, Diag::Unit, Akk);

                    // solve A(k, k) A(k, j) = A(k, j)
                    internal::trsm<target>(
                        Side::Left,
                        scalar_t(1.0), std::move(Tkk),
                        A.sub(k, k, j, j), priority_one,
                        Layout::ColMajor, j-k+1);

                    // send A(k, j) across column A(k+1:mt-1, j)
                    // todo: trsm still operates in ColMajor
                    A.tileBcast(k, j, A.sub(k+1, A_mt-1, j, j), Layout::ColMajor, tag_j);


/*//                #pragma omp task depend(in:column[k]) \
//                                 depend(inout:column[j]) \
//                                 priority(priority_one)*/
//                {
                    // A(k+1:mt-1, j) -= A(k+1:mt-1, k) * A(k, j)
                    internal::gemm<target>(
                            scalar_t(-1.0), A.sub(k+1, A_mt-1, k, k),
                                            A.sub(k, k, j, j),
                            scalar_t(1.0),  A.sub(k+1, A_mt-1, j, j),
                            target_layout, priority_one, j-k+1);

                //}
            }
        }
            // update trailing submatrix, normal priority
            if (k+1+lookahead < A_nt) {
              ///  #pragma omp task depend(in:diag[k]) \
                                 depend(inout:column[k+1+lookahead]) \
                                 depend(inout:listBcastMT_token) \
                                 depend(inout:column[A_nt-1])
                {
                    // swap rows in A(k:mt-1, kl+1:nt-1)
                    int tag_kl1 = k+1+lookahead;
                    internal::permuteRows<Target::HostTask>(
                            Direction::Forward, A.sub(k, A_mt-1, k+1+lookahead, A_nt-1),
                            pivots.at(k), host_layout, priority_zero, tag_kl1, 1);

                    auto Akk = A.sub(k, k, k, k);
                    auto Tkk =
                        TriangularMatrix<scalar_t>(Uplo::Lower, Diag::Unit, Akk);

                    // solve A(k, k) A(k, kl+1:nt-1) = A(k, kl+1:nt-1)
                    internal::trsm<target>(
                        Side::Left,
                        scalar_t(1.0), std::move(Tkk),
                                       A.sub(k, k, k+1+lookahead, A_nt-1),
                        priority_zero, Layout::ColMajor, 1);

                    // send A(k, kl+1:A_nt-1) across A(k+1:mt-1, kl+1:nt-1)
                    BcastListTag bcast_list;
                    for (int64_t j = k+1+lookahead; j < A_nt; ++j) {
                        // send A(k, j) across column A(k+1:mt-1, j)
                        // tag must be distinct from sending left panel
                        const int64_t tag = j + A_mt;
                        bcast_list.push_back({k, j, {A.sub(k+1, A_mt-1, j, j)}, tag});
                    }
                    // todo: trsm still operates in ColMajor
                    A.template listBcastMT<target>(
                        bcast_list, Layout::ColMajor);

                //TODO::RABAB
               /*#pragma omp task depend(in:column[k]) \
               //                 depend(inout:column[k+1+lookahead]) \
               //                 depend(inout:column[A_nt-1])*/
                    // A(k+1:mt-1, kl+1:nt-1) -= A(k+1:mt-1, k) * A(k, kl+1:nt-1)
                    internal::gemm<target>(
                            scalar_t(-1.0), A.sub(k+1, A_mt-1, k, k),
                                            A.sub(k, k, k+1+lookahead, A_nt-1),
                            scalar_t(1.0),  A.sub(k+1, A_mt-1, k+1+lookahead, A_nt-1),
                            target_layout, priority_zero, 1);
                     #if 0
                        for (int i=0; i<A.mt(); i++){
                            if (A.tileIsLocal(3, 2)){
                                if( k==1){
                                    std::cout<<"\n Factore Tile: "<<i<<" of rank: "<<A.mpiRank()<<std::endl;
                                    for(int m=0; m<A.tileMb(i);m++){
                                        for(int n=0; n<A.tileMb(i);n++){
                                            std::cout<<A(3,2)(m,n)<<",";
                                        }
                                        std::cout<<std::endl;
                                    }
                                }
                            }
                        }
                     #endif

                }
            }

      //       #pragma omp task depend(inout:column[k]) \
                                   depend(out:diag[k]) \
                                   priority(priority_one)
          ///  {
           ///     internal::copy<Target::HostTask>( Apanel.sub( 0, 0, 0, 0 ), A.sub( k, k, k, k ));
            ///    Apanel.clear();
            ///}

            //TODO::RABAB ask
            if (target == Target::Devices) {
                #pragma omp task depend(inout:diag[k])
                {
                    if (A.tileIsLocal(k, k) && k+1 < A_nt) {
                        std::set<int> dev_set;
                        A.sub(k+1, A_mt-1, k, k).getLocalDevices(&dev_set);
                        A.sub(k, k, k+1, A_nt-1).getLocalDevices(&dev_set);

                        for (auto device : dev_set) {
                            A.tileUnsetHold(k, k, device);
                            A.tileRelease(k, k, device);
                        }
                    }
                }
                if (is_shared) {
                    #pragma omp task depend(inout:column[k])
                    {
                        for (int64_t i = k+1; i < A_mt; ++i) {
                            if (A.tileIsLocal(i, k)) {
                                A.tileUpdateOrigin(i, k);

                                std::set<int> dev_set;
                                A.sub(i, i, k+1, A_nt-1).getLocalDevices(&dev_set);

                                for (auto device : dev_set) {
                                    A.tileUnsetHold(i, k, device);
                                    A.tileRelease(i, k, device);
                                }
                            }
                        }
                    }
                }
            }


        }
        //#pragma omp taskwait
        A.tileUpdateAllOrigin();
    }

    // Pivot to the left of the panel.
    // todo: Blend into the factorization.
    for (int64_t k = 0; k < min_mt_nt; ++k) {
        if (k > 0) {
            // swap rows in A(k:mt-1, 0:k-1)
            internal::permuteRows<Target::HostTask>(
                Direction::Forward, A.sub(k, A_mt-1, 0, k-1), pivots.at(k),
                host_layout);
        }
    }

    /*#pragma omp parallel
    #pragma omp master
    {
        A.tileLayoutReset();
    }*/
    A.clearWorkspace();
}

} // namespace specialization
} // namespace internal

//------------------------------------------------------------------------------
/// Version with target as template parameter.
/// @ingroup gesv_specialization
///
template <Target target, typename scalar_t>
void getrf_ca(Matrix<scalar_t>& A, Pivots& pivots,
           Options const& opts)
{
    int64_t lookahead;
    try {
        lookahead = opts.at(Option::Lookahead).i_;
        assert(lookahead >= 0);
    }
    catch (std::out_of_range&) {
        lookahead = 1;
    }

    int64_t ib;
    try {
        ib = opts.at(Option::InnerBlocking).i_;
        assert(ib >= 0);
    }
    catch (std::out_of_range&) {
        ib = 16;
    }

    int64_t max_panel_threads;
    try {
        max_panel_threads = opts.at(Option::MaxPanelThreads).i_;
        assert(max_panel_threads >= 1 && max_panel_threads <= omp_get_max_threads());
    }
    catch (std::out_of_range&) {
        max_panel_threads = std::max(omp_get_max_threads()/2, 1);
    }

    internal::specialization::getrf_ca(internal::TargetType<target>(),
                                    A, pivots,
                                    ib, max_panel_threads, lookahead);
}

//------------------------------------------------------------------------------
/// Distributed parallel LU factorization.
///
/// Computes an LU factorization of a general m-by-n matrix $A$
/// using partial pivoting with row interchanges.
///
/// The factorization has the form
/// \[
///     A = P L U
/// \]
/// where $P$ is a permutation matrix, $L$ is lower triangular with unit
/// diagonal elements (lower trapezoidal if m > n), and $U$ is upper
/// triangular (upper trapezoidal if m < n).
///
/// This is the right-looking Level 3 BLAS version of the algorithm.
///
//------------------------------------------------------------------------------
/// @tparam scalar_t
///     One of float, double, std::complex<float>, std::complex<double>.
//------------------------------------------------------------------------------
/// @param[in,out] A
///     On entry, the matrix $A$ to be factored.
///     On exit, the factors $L$ and $U$ from the factorization $A = P L U$;
///     the unit diagonal elements of $L$ are not stored.
///
/// @param[out] pivots
///     The pivot indices that define the permutation matrix $P$.
///
/// @param[in] opts
///     Additional options, as map of name = value pairs. Possible options:
///     - Option::Lookahead:
///       Number of panels to overlap with matrix updates.
///       lookahead >= 0. Default 1.
///     - Option::InnerBlocking:
///       Inner blocking to use for panel. Default 16.
///     - Option::MaxPanelThreads:
///       Number of threads to use for panel. Default omp_get_max_threads()/2.
///     - Option::Target:
///       Implementation to target. Possible values:
///       - HostTask:  OpenMP tasks on CPU host [default].
///       - HostNest:  nested OpenMP parallel for loop on CPU host.
///       - HostBatch: batched BLAS on CPU host.
///       - Devices:   batched BLAS on GPU device.
///
/// TODO: return value
/// @retval 0 successful exit
/// @retval >0 for return value = $i$, $U(i,i)$ is exactly zero. The
///         factorization has been completed, but the factor $U$ is exactly
///         singular, and division by zero will occur if it is used
///         to solve a system of equations.
///
/// @ingroup gesv_computational
///
template <typename scalar_t>
void getrf_ca(Matrix<scalar_t>& A, Pivots& pivots,
           Options const& opts)
{
    Target target;
    try {
        target = Target(opts.at(Option::Target).i_);
    }
    catch (std::out_of_range&) {
        target = Target::HostTask;
    }

    switch (target) {
        case Target::Host:
        case Target::HostTask:
            getrf_ca<Target::HostTask>(A, pivots, opts);
            break;
        case Target::HostNest:
            getrf_ca<Target::HostNest>(A, pivots, opts);
            break;
        case Target::HostBatch:
            getrf_ca<Target::HostBatch>(A, pivots, opts);
            break;
        case Target::Devices:
            getrf_ca<Target::Devices>(A, pivots, opts);
            break;
    }
    // todo: return value for errors?
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void getrf_ca<float>(
    Matrix<float>& A, Pivots& pivots,
    Options const& opts);

template
void getrf_ca<double>(
    Matrix<double>& A, Pivots& pivots,
    Options const& opts);

template
void getrf_ca< std::complex<float> >(
    Matrix< std::complex<float> >& A, Pivots& pivots,
    Options const& opts);

template
void getrf_ca< std::complex<double> >(
    Matrix< std::complex<double> >& A, Pivots& pivots,
    Options const& opts);

} // namespace slate
