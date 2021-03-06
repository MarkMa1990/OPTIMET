// (C) University College London 2017
// This file is part of Optimet, licensed under the terms of the GNU Public License
//
// Optimet is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Optimet is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Optimet. If not, see <http://www.gnu.org/licenses/>.

#include "Types.h"
#include "mpi/FastMatrixMultiply.h"
#include <iostream>

namespace optimet {
namespace mpi {
namespace details {
Matrix<bool> local_interactions(t_int nscatterers, t_int diagonal) {
  assert(nscatterers >= 0);
  if(diagonal < 0)
    return Matrix<bool>::Zero(nscatterers, nscatterers);
  if(diagonal >= nscatterers)
    return Matrix<bool>::Ones(nscatterers, nscatterers);

  Matrix<bool> result = Matrix<bool>::Zero(nscatterers, nscatterers);
  for(t_int d(0); d < nscatterers; ++d) {
    auto const start = std::max(0, d - diagonal);
    auto const end = std::min(nscatterers, d + diagonal + 1);
    result.row(d).segment(start, end - start).fill(true);
  }
  return result;
}

Vector<t_int> vector_distribution(t_int nscatterers, t_int nprocs) {
  assert(nprocs > 0);
  // horizontal consists of horizontal bands colored with the relevant process
  auto const N = std::min(nscatterers, nprocs);
  Vector<t_int> result = Vector<t_int>::Constant(nscatterers, N);
  for(t_int i(0); i < N; ++i) {
    auto const divided = nscatterers / N;
    auto const remainder = nscatterers % N;
    auto const loc = divided * i + std::min(remainder, i);
    auto const size = divided + (remainder > i ? 1 : 0);
    result.segment(loc, size).fill(i);
  }
  assert(N == 0 or (result.array() < N).all());
  return result;
}

std::vector<std::set<t_uint>>
graph_edges(Matrix<bool> const &considered, Vector<t_int> const &vecdist) {
  assert(considered.rows() == considered.cols());
  assert(considered.rows() == vecdist.size());
  auto const nprocs = vecdist.maxCoeff() + 1;
  std::vector<std::set<t_uint>> results(nprocs);
  for(t_uint i(0); i < considered.rows(); ++i)
    for(t_uint j(0); j < considered.cols(); ++j) {
      if(considered(i, j) == false)
        continue;
      results[vecdist(j)].insert(vecdist(i));
      results[vecdist(i)].insert(vecdist(j));
    }
  return results;
}
}

Vector<t_complex> FastMatrixMultiply::operator()(Vector<t_complex> const &in) const {
  Vector<t_complex> result(cols());
  operator()(in, result);
  return result;
}

Vector<t_complex> FastMatrixMultiply::transpose(Vector<t_complex> const &in) const {
  Vector<t_complex> result(rows());
  transpose(in, result);
  return result;
}

void FastMatrixMultiply::operator()(Vector<t_complex> const &input, Vector<t_complex> &out) const {
  out.fill(0);
  /************* START FIRST COMMUNICATION **********/
  // first communicate input data to other processes
  Vector<t_complex> distribute_buffer;
  auto distribute_request = distribute_input_.send(input, distribute_buffer);
  // while data is being sent, we compute stuff with local input...
  auto const local_input = reconstruct(local_indices_, input);
  auto const nl_computations = local_fmm_(local_input);

  /************* START SECOND COMMUNICATION **********/
  // and send the result of the local computations
  Vector<t_complex> send_buffer, computation_buffer;
  auto reduction_request =
      reduce_computation_.send(nl_computations, send_buffer, computation_buffer);
  // now we get the inputs from other processes
  mpi::wait(std::move(distribute_request));
  /************* FINISHED FIRST COMMUNICATION **********/

  // And synthesize the input for non-local fmm
  auto const nonlocal_input = distribute_input_.synthesize(distribute_buffer);
  // we can now compute stuff involving non-local information
  auto const nl_out = nonlocal_fmm_(nonlocal_input);
  // Non-local output may be missing some bits
  // So we need to reconstruct it
  reconstruct(nonlocal_indices_, nl_out, out);

  // we receive the stuff computed elsewhere
  mpi::wait(std::move(reduction_request));
  /************* FINISHED SECOND COMMUNICATION **********/

  // and reduce over all results
  reduce_computation_.reduce(out, computation_buffer);
}

void FastMatrixMultiply::transpose(Vector<t_complex> const &input, Vector<t_complex> &out) const {
  out.fill(0);
  /************* START FIRST COMMUNICATION **********/
  // first communicate input data to other processes
  Vector<t_complex> distribute_buffer;
  auto distribute_request = distribute_input_.send(input, distribute_buffer);
  // while data is being sent, we compute stuff with local input...
  auto const local_input = reconstruct(local_indices_, input);
  auto const nl_computations = transpose_local_fmm_.transpose(local_input);

  /************* START SECOND COMMUNICATION **********/
  // and send the result of the local computations
  Vector<t_complex> send_buffer, computation_buffer;
  auto reduction_request =
      reduce_computation_.send(nl_computations, send_buffer, computation_buffer);
  // now we get the inputs from other processes
  mpi::wait(std::move(distribute_request));
  /************* FINISHED FIRST COMMUNICATION **********/

  // And synthesize the input for non-local fmm
  auto const nonlocal_input = distribute_input_.synthesize(distribute_buffer);
  // we can now compute stuff involving non-local information
  auto const nl_out = transpose_nonlocal_fmm_.transpose(nonlocal_input);
  // Non-local output may be missing some bits
  // So we need to reconstruct it
  reconstruct(nonlocal_indices_, nl_out, out);

  // we receive the stuff computed elsewhere
  mpi::wait(std::move(reduction_request));
  /************* FINISHED SECOND COMMUNICATION **********/

  // and reduce over all results
  reduce_computation_.reduce(out, computation_buffer);
}

namespace {
Vector<int> compute_sizes(std::vector<Scatterer> const &scatterers) {
  Vector<int> result(scatterers.size());
  for(Vector<int>::Index i(0); i < result.size(); ++i)
    result(i) = 2 * scatterers[i].nMax * (scatterers[i].nMax + 2);
  return result;
}
}

FastMatrixMultiply::FastMatrixMultiply(ElectroMagnetic const &em_background, t_real wavenumber,
                                       std::vector<Scatterer> const &scatterers,
                                       Matrix<bool> const &locals,
                                       GraphCommunicator const &distribute_comm,
                                       GraphCommunicator const &reduce_comm,
                                       Vector<t_int> const &vector_distribution,
                                       Communicator const &comm)
    : local_fmm_(em_background, wavenumber, scatterers,
                 locals.array() &&
                     (vector_distribution.transpose().array() == comm.rank())
                         .replicate(vector_distribution.size(), 1)),
      nonlocal_fmm_(em_background, wavenumber, scatterers,
                    (locals.array() == false) &&
                        (vector_distribution.array() == comm.rank())
                            .replicate(1, vector_distribution.size())),
      transpose_local_fmm_(em_background, wavenumber, scatterers,
                           locals.transpose().array() &&
                               (vector_distribution.array() == comm.rank())
                                   .replicate(1, vector_distribution.size())),
      transpose_nonlocal_fmm_(em_background, wavenumber, scatterers,
                              (locals.transpose().array() == false) &&
                                  (vector_distribution.transpose().array() == comm.rank())
                                      .replicate(vector_distribution.size(), 1)),
      distribute_input_(distribute_comm, locals.array() == false, vector_distribution, scatterers),
      reduce_computation_(reduce_comm, locals.array(), vector_distribution, scatterers) {

  auto const owned = vector_distribution.array() == comm.rank();
  auto const sizes = compute_sizes(scatterers);
  std::vector<t_uint> const self = {comm.rank()};

  auto const band =
      (vector_distribution.array() == comm.rank()).replicate(1, vector_distribution.size());
  auto const nl_out = ((locals.array() == false) && band).rowwise().any();
  auto const local_in = (locals.array() && band.transpose()).colwise().any();
  for(t_uint j(0), iloc(0), nl_loc(0), local_loc(0); j < nl_out.size(); ++j) {
    if(not owned(j))
      continue;
    if(nl_out(j) == true) {
      nonlocal_indices_.push_back(
          std::array<t_uint, 3>{{static_cast<t_uint>(sizes[j]), nl_loc, iloc}});
      nl_loc += sizes[j];
    }
    if(local_in(j) == true) {
      local_indices_.push_back(
          std::array<t_uint, 3>{{static_cast<t_uint>(sizes[j]), iloc, local_loc}});
      local_loc += sizes[j];
    }
    iloc += sizes[j];
  }
}

FastMatrixMultiply::DistributeInput::DistributeInput(GraphCommunicator const &comm,
                                                     Matrix<bool> const &allowed,
                                                     Vector<int> const &distribution,
                                                     Vector<int> const &sizes)
    : comm(comm) {
  auto const neighborhood = comm.neighborhood();
  // inputs required for local computations
  auto const inputs = [&distribution, &allowed](t_uint rank) {
    return (allowed.array() && (distribution.array() == rank).replicate(1, distribution.size()))
        .colwise()
        .any();
  };
  // inputs required by this process
  auto const local_inputs = inputs(comm.rank());

  auto const owned = (distribution.array() == comm.rank()).eval();
  for(decltype(neighborhood)::size_type i(0), sloc(0); i < neighborhood.size(); ++i) {
    // Helps reconstruct data sent from other procs
    auto const nl_owned = (distribution.array() == neighborhood[i]).eval();
    for(t_uint j(0), iloc(0); j < local_inputs.size(); ++j) {
      if(nl_owned(j) == true and local_inputs(j) == true)
        relocate_receive.push_back(
            std::array<t_uint, 3>{{static_cast<t_uint>(sizes[j]), sloc, iloc}});
      if(nl_owned(j) == true)
        sloc += sizes[j];
      if(local_inputs(j) == true)
        iloc += sizes[j];
    }

    // Amount of data to receive from each proc
    receive_counts.push_back(nl_owned.select(sizes, Vector<int>::Zero(sizes.size())).sum());
  }
  synthesis_size = local_inputs.transpose().select(sizes, Vector<int>::Zero(sizes.size())).sum();
}

FastMatrixMultiply::DistributeInput::DistributeInput(GraphCommunicator const &comm,
                                                     Matrix<bool> const &allowed,
                                                     Vector<int> const &distribution,
                                                     std::vector<Scatterer> const &scatterers)
    : DistributeInput(comm, allowed, distribution, compute_sizes(scatterers)){};

FastMatrixMultiply::ReduceComputation::ReduceComputation(GraphCommunicator const &comm,
                                                         Matrix<bool> const &allowed,
                                                         Vector<int> const &distribution,
                                                         Vector<int> const &sizes)
    : comm(comm) {
  auto const neighborhood = comm.neighborhood();
  auto const comps = [&distribution, &allowed](t_uint rank) {
    return (allowed.array() &&
            (distribution.transpose().array() == rank).replicate(distribution.size(), 1))
        .rowwise()
        .any();
  };
  auto const owned = [&distribution](t_uint rank) { return distribution.array() == rank; };
  relocate_receive =
      FastMatrixMultiply::reconstruct(neighborhood, owned(comm.rank()), sizes, comps);
  relocate_send = FastMatrixMultiply::reconstruct(neighborhood, comps(comm.rank()), sizes, owned);
  for(decltype(neighborhood)::size_type i(0); i < neighborhood.size(); ++i) {
    // Amount of data to send to each proc
    send_counts.push_back((comps(comm.rank()) && owned(neighborhood[i]))
                              .select(sizes, Vector<int>::Zero(sizes.size()))
                              .sum());
    // Amount of data to receive from each proc
    receive_counts.push_back((comps(neighborhood[i]) && owned(comm.rank()))
                                 .select(sizes, Vector<int>::Zero(sizes.size()))
                                 .sum());
  }
  message_size = std::accumulate(send_counts.begin(), send_counts.end(), 0);
}

FastMatrixMultiply::ReduceComputation::ReduceComputation(GraphCommunicator const &comm,
                                                         Matrix<bool> const &allowed,
                                                         Vector<int> const &distribution,
                                                         std::vector<Scatterer> const &scatterers)
    : ReduceComputation(comm, allowed, distribution, compute_sizes(scatterers)){};
}
} // optimet namespace
