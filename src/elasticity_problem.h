// Copyright (C) 2017-2019 Chris N. Richardson and Garth N. Wells
//
// This file is part of FEniCS-miniapp (https://www.fenicsproject.org)
//
// SPDX-License-Identifier:    MIT

#pragma once

#include "Elasticity.h"
#include <Eigen/Dense>
#include <dolfinx/common/Timer.h>
#include <dolfinx/fem/DirichletBC.h>
#include <dolfinx/fem/DofMap.h>
#include <dolfinx/fem/Form.h>
#include <dolfinx/fem/assembler.h>
#include <dolfinx/fem/utils.h>
#include <dolfinx/function/Function.h>
#include <dolfinx/function/FunctionSpace.h>
#include <dolfinx/la/PETScMatrix.h>
#include <dolfinx/la/PETScVector.h>
#include <dolfinx/la/VectorSpaceBasis.h>
#include <dolfinx/la/utils.h>
#include <dolfinx/mesh/Geometry.h>
#include <dolfinx/mesh/Mesh.h>
#include <memory>
#include <utility>

namespace elastic
{
// Function to compute the near nullspace for elasticity - it is made up
// of the six rigid body modes

dolfinx::la::VectorSpaceBasis
build_near_nullspace(const dolfinx::function::FunctionSpace& V)
{
  // Get subspaces
  auto V0 = V.sub({0});
  auto V1 = V.sub({1});
  auto V2 = V.sub({2});

  // Create vectors for nullspace basis
  std::vector<std::shared_ptr<dolfinx::la::PETScVector>> basis_vec;
  for (std::size_t i = 0; i < 6; ++i)
  {
    basis_vec.push_back(
        std::make_shared<dolfinx::la::PETScVector>(*V.dofmap()->index_map));
  }

  {
    // Unwrap the PETSc Vec objects to allow array (Eigen) access
    std::vector<dolfinx::la::VecWrapper> basis;
    for (auto vec : basis_vec)
      basis.push_back(dolfinx::la::VecWrapper(vec->vec()));

    // x0, x1, x2 translations
    V0->dofmap()->set(basis[0].x, 1.0);
    V1->dofmap()->set(basis[1].x, 1.0);
    V2->dofmap()->set(basis[2].x, 1.0);

    // Rotations
    V0->set_x(basis[3].x, -1.0, 1);
    V1->set_x(basis[3].x, 1.0, 0);

    V0->set_x(basis[4].x, 1.0, 2);
    V2->set_x(basis[4].x, -1.0, 0);

    V2->set_x(basis[5].x, 1.0, 1);
    V1->set_x(basis[5].x, -1.0, 2);
  }

  // Create vector space and orthonormalize
  dolfinx::la::VectorSpaceBasis vector_space(basis_vec);
  vector_space.orthonormalize();
  return vector_space;
}

std::tuple<dolfinx::la::PETScMatrix, dolfinx::la::PETScVector,
           std::shared_ptr<dolfinx::function::Function>>
problem(std::shared_ptr<dolfinx::mesh::Mesh> mesh)
{
  dolfinx::common::Timer t0("ZZZ FunctionSpace");

  std::shared_ptr<dolfinx::function::FunctionSpace> V
      = dolfinx::fem::create_functionspace(
          create_functionspace_form_Elasticity_a, "u", mesh);

  t0.stop();

  dolfinx::common::Timer t1("ZZZ Assemble prep");

  // Define variational forms

  std::shared_ptr<dolfinx::fem::Form> L
      = dolfinx::fem::create_form(create_form_Elasticity_L, {V});

  std::shared_ptr<dolfinx::fem::Form> a
      = dolfinx::fem::create_form(create_form_Elasticity_a, {V, V});

  // Attach 'coordinate mapping' to mesh
  auto cmap = a->coordinate_mapping();
  mesh->geometry().coord_mapping = cmap;

  // Define boundary condition
  auto u0 = std::make_shared<dolfinx::function::Function>(V);
  dolfinx::la::VecWrapper _u0(u0->vector().vec());
  _u0.x.setZero();
  _u0.restore();

  const Eigen::Array<std::int32_t, Eigen::Dynamic, 1> bdofs
      = dolfinx::fem::locate_dofs_geometrical(
          {*V}, [](auto& x) { return x.row(1) < 1.0e-8; });

  // Bottom (x[1] = 0) surface
  auto bc = std::make_shared<dolfinx::fem::DirichletBC>(u0, bdofs);

  // Define coefficients
  auto f = std::make_shared<dolfinx::function::Function>(V);
  f->interpolate([](auto& x) {
    auto dx = x.row(0) - 0.5;
    auto dz = x.row(2) - 0.5;
    auto r = dx * dx + dz * dz;
    Eigen::Array<double, 3, Eigen::Dynamic, Eigen::RowMajor> values(3,
                                                                    x.cols());
    values.row(0) = -dz * r.sqrt() * x.row(1);
    values.row(1) = 1.0;
    values.row(2) = dx * r.sqrt() * x.row(1);
    return values;
  });

  L->set_coefficients({{"f", f}});

  t1.stop();

  dolfinx::la::PETScMatrix A2 = dolfinx::fem::create_matrix(*a);
  dolfinx::fem::assemble_matrix(A2.mat(), *a, {});
  MatAssemblyBegin(A2.mat(), MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(A2.mat(), MAT_FINAL_ASSEMBLY);

  // Create matrices and vector, and assemble system
  dolfinx::la::PETScMatrix A = dolfinx::fem::create_matrix(*a);
  dolfinx::la::PETScVector b(*L->function_space(0)->dofmap()->index_map);

  MatZeroEntries(A.mat());

  dolfinx::common::Timer t2("ZZZ Assemble matrix");
  dolfinx::fem::assemble_matrix(A.mat(), *a, {bc});
  dolfinx::fem::add_diagonal(A.mat(), *V, {bc});
  MatAssemblyBegin(A.mat(), MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(A.mat(), MAT_FINAL_ASSEMBLY);
  t2.stop();

  VecSet(b.vec(), 0.0);
  VecGhostUpdateBegin(b.vec(), INSERT_VALUES, SCATTER_FORWARD);
  VecGhostUpdateEnd(b.vec(), INSERT_VALUES, SCATTER_FORWARD);

  dolfinx::common::Timer t3("ZZZ Assemble vector");
  dolfinx::fem::assemble_vector(b.vec(), *L);
  dolfinx::fem::apply_lifting(b.vec(), {a}, {{bc}}, {}, 1.0);
  VecGhostUpdateBegin(b.vec(), ADD_VALUES, SCATTER_REVERSE);
  VecGhostUpdateEnd(b.vec(), ADD_VALUES, SCATTER_REVERSE);
  dolfinx::fem::set_bc(b.vec(), {bc}, nullptr);
  t3.stop();

  dolfinx::common::Timer t4("ZZZ Create near-nullspace");

  // Create Function to hold solution
  auto u = std::make_shared<dolfinx::function::Function>(V);

  // Build near-nullspace and attach to matrix
  dolfinx::la::VectorSpaceBasis nullspace = build_near_nullspace(*V);
  A.set_near_nullspace(nullspace);
  t4.stop();

  // Create PETSc nullspace
  MatNullSpace petsc_ns
      = dolfinx::la::create_petsc_nullspace(mesh->mpi_comm(), nullspace);
  PetscBool isNull;
  MatNullSpaceTest(petsc_ns, A2.mat(), &isNull);
  if (isNull == PETSC_TRUE)
    std::cout << "Is a nullspace: " << isNull << std::endl;
  else
    std::cout << "Not a nullspace: " << isNull << std::endl;

  return std::tuple<dolfinx::la::PETScMatrix, dolfinx::la::PETScVector,
                    std::shared_ptr<dolfinx::function::Function>>(
      std::move(A), std::move(b), u);
}
} // namespace elastic
