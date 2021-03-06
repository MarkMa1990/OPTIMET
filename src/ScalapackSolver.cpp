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

#include "ScalapackSolver.h"
#include "scalapack/BroadcastToOutOfContext.h"
#include "scalapack/LinearSystemSolver.h"

namespace optimet {
namespace solver {

std::tuple<scalapack::Matrix<t_complex>, scalapack::Matrix<t_complex>>
Scalapack::parallel_input() const {
  auto const nMax = geometry->nMax();
  auto const N = 2 * nMax * (nMax + 2) * geometry->objects.size();
  scalapack::Matrix<t_complex> Aparallel(context(), {N, N}, block_size());
  if(Aparallel.size() > 0)
    Aparallel.local() = S;
  scalapack::Matrix<t_complex> bparallel(context(), {N, 1}, block_size());
  if(bparallel.local().size() > 0)
    bparallel.local() = Q;
  return std::make_tuple(Aparallel, bparallel);
}

void Scalapack::solve(Vector<t_complex> &X_sca_, Vector<t_complex> &X_int_) const {
  if(context().is_valid()) {
    auto input = parallel_input();
    // Now the actual work
    auto const gls_result =
        scalapack::general_linear_system(std::get<0>(input), std::get<1>(input));
    if(std::get<1>(gls_result) != 0)
      throw std::runtime_error("Error encountered while solving the linear system");
    // Transfer back to root
    X_sca_ = gather_all_source_vector(std::get<0>(gls_result));
    PreconditionedMatrix::unprecondition(X_sca_, X_int_);
  }
  if(context().size() != communicator().size()) {
    broadcast_to_out_of_context(X_sca_, context(), communicator());
    broadcast_to_out_of_context(X_int_, context(), communicator());
  }
}

void Scalapack::update() {
  Q = distributed_source_vector(source_vector(*geometry, incWave), context(), block_size());
  S = preconditioned_scattering_matrix(*geometry, incWave, context(), block_size());
}
}
}
