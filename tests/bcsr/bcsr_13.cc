// check FEEvaluation::read_dof_values() and distribute_local_to_global()
// for BCSR sparse vectors. To that end,
// 1) reorder DoFs based on support point locations
// 2) use 1 cell and quadratic FE
// 3) block by number of DoFs in one direction == 3
// 4) add couple of columns

#include <deal.II/distributed/tria.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/mapping_q1.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/matrix_free/fe_evaluation.h>
#include <deal.II/matrix_free/matrix_free.h>
#include <deal.II/numerics/vector_tools.h>

#include "bcsr_helper.h"
#include <RFAStDFT/block_csr_matrix.h>

#include <fstream>
#include <iostream>

using namespace dealii;
using namespace RealFAStDFT;



template <int dim,
          int fe_degree,
          int n_q_points_1d = fe_degree + 1,
          typename Number = double,
          int n_components = 1>
class MatrixFreeTest
{
public:
  MatrixFreeTest(const std::shared_ptr<const MatrixFree<dim, Number>> &data)
    : data(data)
  {
  }

  void vmult(BlockCSRMatrix<Number> &dst,
             const BlockCSRMatrix<Number> &src) const
  {
    dst = 0;
    data->cell_loop(&MatrixFreeTest::local_apply_cell,
                    this,
                    dst,
                    src,
                    /*zero_dst_vector*/ false);
  }

private:

  void local_apply_cell(
    const MatrixFree<dim, Number> &/*data*/,
    BlockCSRMatrix<Number> &dst,
    const BlockCSRMatrix<Number> &src,
    const std::pair<unsigned int, unsigned int> &cell_range) const
  {
    FEEvaluation<dim, fe_degree, n_q_points_1d, n_components, Number> phi(
      *data);

    BlockCSRMatrixIterators::RowsAccessor<Number, true> src_row_accessor(&src);
    BlockCSRMatrixIterators::RowsAccessor<Number, false> dst_row_accessor(&dst);

    const auto &dof_info = data->get_dof_info();
    std::vector<unsigned int> my_rows;
    my_rows.reserve(phi.dofs_per_component *
                    VectorizedArray<Number>::n_array_elements);

    unsigned int nonzero_columns = 0;
    for (unsigned int cell = cell_range.first; cell < cell_range.second; ++cell)
      {
        // collect DoFs on all cell blocks
        dof_info.get_dof_indices_on_cell_batch(
          my_rows, cell, true /*apply_constraints*/);

        deallog << "Rows on cell: " << cell << std::endl;
        for (auto r : my_rows)
          deallog << " " << r;
        deallog << std::endl;

        src_row_accessor.reinit(my_rows);
        dst_row_accessor.reinit(my_rows);

        phi.reinit(cell);

        bool iterate = true;
        while (iterate)
          {
            const auto src_col = src_row_accessor.current_column();
            deallog << "Column: " << src_col << std::endl;
            // make sure to accessors point to the same column
            bool iterate_dst = true;
            while (iterate_dst &&
                   (src_col != dst_row_accessor.current_column()))
              iterate_dst = dst_row_accessor.advance();

            // at this point we should really have matching columns:
            Assert(
              iterate_dst && (src_col == dst_row_accessor.current_column()),
              ExcMessage(
                std::to_string(src_row_accessor.current_column()) +
                " != " + std::to_string(dst_row_accessor.current_column())));

            // now do the standard matrix-free operations using accessors
            {
              phi.read_dof_values(src_row_accessor);
              phi.evaluate(true, true, false);
              for (unsigned int q = 0; q < phi.n_q_points; ++q)
                {
                  phi.submit_gradient(0.5 * phi.get_gradient(q), q);
                  phi.submit_value(phi.get_value(q), q);
                }
              phi.integrate(true, true);
              phi.distribute_local_to_global(dst_row_accessor);
            }

            // finally advance the src accessor:
            iterate = src_row_accessor.advance();
            ++nonzero_columns;
          }

      } // loop over all cells

    deallog << "Nonzero columns over " << cell_range.second - cell_range.first
            << " cells: " << nonzero_columns << std::endl;
  }

  const std::shared_ptr<const MatrixFree<dim, Number>> data;
};



template <int dim,
          typename Number = double,
          int fe_degree = 2,
          int n_q_points_1d = fe_degree+1>
void test(const unsigned int n_cells = 1)
{
  MPI_Comm mpi_communicator(MPI_COMM_WORLD);
  const unsigned int this_mpi_core =
    dealii::Utilities::MPI::this_mpi_process(mpi_communicator);
  parallel::distributed::Triangulation<dim> triangulation(
    mpi_communicator,
    Triangulation<dim>::limit_level_difference_at_vertices,
    parallel::distributed::Triangulation<dim>::construct_multigrid_hierarchy);

  // Setup system
  {
    std::vector<unsigned int> repetitions(dim, 1);
    repetitions[0] = n_cells;
    const Point<dim> p1;
    Point<dim> p2;
    for (unsigned int d = 1; d < dim; ++d)
      p2[d] = 1.;
    p2[0] = n_cells;
    GridGenerator::subdivided_hyper_rectangle(
      triangulation, repetitions, p1, p2, true);
  }

  DoFHandler<dim> dh(triangulation);

  FE_Q<dim> fe(fe_degree);
  dh.distribute_dofs(fe);

  const std::vector<unsigned int> row_blocks = renumber_based_on_nodes(dh);

  // now test with evaluating
  IndexSet locally_relevant_dofs;
  DoFTools::extract_locally_relevant_dofs(dh, locally_relevant_dofs);

  AffineConstraints<double> constraints;
  constraints.reinit(locally_relevant_dofs);
  DoFTools::make_hanging_node_constraints(dh, constraints);
  VectorTools::interpolate_boundary_values(
    dh, 0 /*left side*/, ZeroFunction<dim>(), constraints);

  constraints.close();

  deallog << "Constraints:" << std::endl;
  constraints.print(deallog.get_file_stream());

  const auto n_row_blocks = row_blocks.size();

  const std::vector<unsigned int> col_blocks = {{2, 1, 3}};

  DynamicSparsityPattern sp_src(n_row_blocks, col_blocks.size());
  DynamicSparsityPattern sp_dst(n_row_blocks, col_blocks.size());

  // after renumbering we know that we have fe_degree+1 nodes with the same x
  // coordinate in one row block.
  // FIXME: use setup_1d_sparsity() to get more interesting blocking.

  // make 3 blocks so that on one cell we have
  // 1: both dst/src non-empty
  {
    const unsigned int col = 0;
    for (unsigned int i = 0; i < n_row_blocks; ++i)
      {
        sp_src.add(i, col);
        sp_dst.add(i, col);
      }
  }

  // 2: both dst/src empty (let this be column 1)

  // 3: some blocks in src are empty, whereas dst is of course full
  {
    const unsigned int col = 2;
    sp_src.add(0, col);
    sp_src.add(1, col);
    for (unsigned int i = 0; i < n_row_blocks; ++i)
      sp_dst.add(i, col);
  }

  std::shared_ptr<BlockIndices> rows =
    std::make_shared<BlockIndices>(row_blocks);
  std::shared_ptr<BlockIndices> cols =
    std::make_shared<BlockIndices>(col_blocks);

  const types::global_dof_index full_rows = dh.n_dofs();
  const types::global_dof_index full_cols = std::accumulate(
    col_blocks.begin(), col_blocks.end(), types::global_dof_index(0));

  deallog << "Sparsity src:" << std::endl;
  sp_src.print(deallog.get_file_stream());
  deallog << "Sparsity dst:" << std::endl;
  sp_dst.print(deallog.get_file_stream());

  // prepare MF data
  std::shared_ptr<MatrixFree<dim, Number>> fine_level_data =
    std::make_shared<MatrixFree<dim, Number>>();
  {
    typename MatrixFree<dim, Number>::AdditionalData fine_level_additional_data;
    fine_level_additional_data.mapping_update_flags =
      update_values | update_gradients | update_JxW_values |
      update_quadrature_points;
    fine_level_additional_data.overlap_communication_computation = false;
    fine_level_additional_data.tasks_parallel_scheme =
      MatrixFree<dim, Number>::AdditionalData::none;
    fine_level_data->reinit(
      dh, constraints, QGauss<1>(n_q_points_1d), fine_level_additional_data);
  }

  const std::shared_ptr<const dealii::Utilities::MPI::Partitioner>
    bcsr_row_part = fine_level_data->get_vector_partitioner();

  // setup matrices
  BlockCSRMatrix<Number> src, dst;
  src.reinit(sp_src, rows, cols, bcsr_row_part);
  dst.reinit(sp_dst, rows, cols, bcsr_row_part);

  deallog << "Internal sparsity src:" << std::endl;
  src.get_sparsity_pattern().print(deallog.get_file_stream());

  deallog << "Internal sparsity dst:" << std::endl;
  dst.get_sparsity_pattern().print(deallog.get_file_stream());

  // randomize content of matrices
  const auto randomize_mat = [](BlockCSRMatrix<Number> &mat) {
    for (unsigned int i = 0; i < mat.n_local_row_blocks(); ++i)
      {
        const auto M = mat.get_row_blocks()->block_size(i);
        for (auto it = mat.begin_local(i); it != mat.end_local(i); ++it)
          {
            const auto j = it->column();
            const auto N = mat.get_col_blocks()->block_size(j);
            for (unsigned int jj = 0; jj < N; ++jj)
              for (unsigned int ii = 0; ii < M; ++ii)
                *(it->data() +
                  BlockCSRMatrix<double>::local_index(ii, jj, M, N)) =
                  Utilities::generate_normal_random_number(0, 0.2);
          }
      }
  };

  randomize_mat(src);

  MatrixFreeTest<dim, fe_degree, n_q_points_1d, Number> mf_test(
    fine_level_data);
  mf_test.vmult(dst, src);


  // now do the same using full serial vectors
  LAPACKFullMatrix<Number> full_src(full_rows, full_cols),
    full_dst(full_rows, full_cols), full_diff(full_rows, full_cols);

  FEEvaluation<dim, fe_degree, n_q_points_1d, 1, double> phi(*fine_level_data);

  src.copy_to(full_src);

  LinearAlgebra::distributed::Vector<Number> src_col(bcsr_row_part);
  LinearAlgebra::distributed::Vector<Number> dst_col(bcsr_row_part);

  const auto loc_w_ghost = bcsr_row_part->local_size() + bcsr_row_part->n_ghost_indices();

  full_dst = 0.;

  for (unsigned int cell = 0; cell < fine_level_data->n_macro_cells(); ++cell)
    {
      phi.reinit(cell);
      for (unsigned int j = 0; j < full_cols; ++j)
        {
          for (unsigned int i = 0; i < loc_w_ghost; ++i)
            src_col.local_element(i) =
              full_src(bcsr_row_part->local_to_global(i), j);

          dst_col = 0.;

          phi.read_dof_values(src_col);
          phi.evaluate(true, true, false);
          for (unsigned int q = 0; q < phi.n_q_points; ++q)
            {
              phi.submit_gradient(0.5 * phi.get_gradient(q), q);
              phi.submit_value(phi.get_value(q), q);
            }
          phi.integrate(true, true);
          phi.distribute_local_to_global(dst_col);

          for (unsigned int i = 0; i < loc_w_ghost; ++i)
            full_dst(bcsr_row_part->local_to_global(i), j) +=
              dst_col.local_element(i);
        } // loop over all columns
    }     // loop over all cells

  Utilities::MPI::sum(full_dst, mpi_communicator, full_dst);

  dst.copy_to(full_diff);
  full_diff.add(-1, full_dst);

  deallog << "frobenius_norm src: " << full_src.frobenius_norm() << std::endl
          << "frobenius_norm dst: " << full_dst.frobenius_norm() << std::endl
          << "linfty_norm diff:   " << full_diff.linfty_norm() << std::endl;

  // print grid and DoFs for visual inspection
  if (dim == 2)
    {
      std::map<types::global_dof_index, Point<dim>> support_points;
      MappingQ1<dim> mapping;
      DoFTools::map_dofs_to_support_points(mapping, dh, support_points);

      const std::string base_filename =
        "grid" + dealii::Utilities::int_to_string(dim) + "_p" +
        dealii::Utilities::int_to_string(this_mpi_core);

      const std::string filename = base_filename + ".gp";
      std::ofstream f(filename.c_str());

      f << "set terminal png size 400,410 enhanced font \"Helvetica,8\""
        << std::endl
        << "set output \"" << base_filename << ".png\"" << std::endl
        << "set size square" << std::endl
        << "set view equal xy" << std::endl
        << "unset xtics" << std::endl
        << "unset ytics" << std::endl
        << "unset grid" << std::endl
        << "unset border" << std::endl
        << "plot '-' using 1:2 with lines notitle, '-' with labels point pt 2 "
           "offset 1,1 notitle"
        << std::endl;
      GridOut().write_gnuplot(triangulation, f);
      f << "e" << std::endl;

      /*
      // to make life easier, filter out all support points which are not in
      // the sets:
      for (auto it = support_points.cbegin(); it != support_points.cend(); )
        {
          if (local_support[i].is_element(it->first))
            ++it;
          else
            support_points.erase(it++);
        }
      */

      DoFTools::write_gnuplot_dof_support_point_info(f, support_points);

      f << "e" << std::endl;
    }

  dh.clear();
  deallog << "Ok" << std::endl;
}

int main(int argc, char **argv)
{
  dealii::Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);

  unsigned int myid = dealii::Utilities::MPI::this_mpi_process(MPI_COMM_WORLD);
  const unsigned int n_procs =
    dealii::Utilities::MPI::n_mpi_processes(MPI_COMM_WORLD);

  std::string deallogname = "output" + dealii::Utilities::int_to_string(myid);
  std::ofstream logfile(deallogname);
  deallog.attach(logfile, /*do not print job id*/ false);
  deallog.depth_console(0);

  test<2>();

  logfile.close();

  MPI_Barrier(MPI_COMM_WORLD);

  if (myid == 0)
    for (unsigned int p = 0; p < n_procs; ++p)
      {
        std::string deallogname =
          "output" + dealii::Utilities::int_to_string(p);
        std::ifstream f(deallogname);
        std::string line;
        while (std::getline(f, line))
          std::cout << p << ":" << line << std::endl;
      }

  return 0;
}
