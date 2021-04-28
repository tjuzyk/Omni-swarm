#pragma once
#include <iostream>
#include "ceres/program.h"
#include <ceres/ceres.h>
#include <map>
#include "ceres/evaluator.h"
#include <chrono>
#include <ctime>


namespace DSLAM {
class TicToc
{
  public:
    TicToc()
    {
        tic();
    }

    void tic()
    {
        start = std::chrono::system_clock::now();
    }

    double toc()
    {
        end = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        return elapsed_seconds.count() * 1000;
    }

  private:
    std::chrono::time_point<std::chrono::system_clock> start, end;
};

class DistributedSolver {
protected:
    std::vector<double*> local_poses;
    std::vector<double*> remote_poses;
    std::vector<std::pair<ceres::CostFunction * , std::vector<double*> >> residuals;
    ceres::internal::Evaluator * evaluator = nullptr;
    ceres::internal::Program * reduced_program = nullptr;
    std::vector<double*> removed_parameter_blocks;
    double fixed_cost;
    std::vector<double> reduced_parameters;
    int num_residuals_;
    int num_parameters_;
    ceres::internal::ProblemImpl* problem_impl;

    //These values are at linearized point!
    ceres::Vector x, residual, gradient;
    
    ceres::internal::SparseMatrix * jacobian = nullptr;
    ceres::Matrix J, Jt;
    ceres::Matrix H,g;

    bool need_setup = true;
    double evaluation_with_jacobian_time = 0;
    double iteration_time;
    int iterations = 0;
public:
    void print_reduced_parameters() {
        ceres::Vector x_;
        x_.resize(num_parameters_);
        memcpy(x_.data(), reduced_parameters.data(), num_parameters_*sizeof(double));
        std::cout << "print_reduced_parameters" << x_.transpose() << std::endl;
    }
    DistributedSolver();
    virtual void set_local_poses(std::vector<double*> poses);
    virtual void set_remote_poses(std::vector<double*> poses);

    virtual void update_remote_poses(std::vector<double*> poses) {};
    
    virtual void set_fixed_poses(std::vector<double*> poses);
    virtual void add_residual(ceres::CostFunction * cost_function, std::vector<double*> poses, bool is_huber_norm = false);
    virtual void linearization() {};
    virtual void setup();
    virtual double cost() const = 0;
    virtual double iteration(bool linearization = true) {};

    virtual std::vector<ceres::Vector> get_last_local_states() {};

    virtual double get_x_jacobian_residual(
        ceres::Vector & x_,
        ceres::Vector & residual,
        ceres::Vector & gradient,
        ceres::internal::SparseMatrix * & jacobian_
    ) const;
};

// class ADMMDistributed : public DistributedSolver {
// public:
//     ADMMDistributed() {}
//     virtual void set_local_poses(std::vector<double*> poses);
//     virtual void set_remote_poses(std::vector<double*> poses);
//     virtual void add_residual(ceres::CostFunction * cost_function, bool is_huber_norm = false);
//     virtual void solve();
// };

class DGSSolver: public  DistributedSolver{
    ceres::Vector delta_last;
    ceres::Vector x_last;
public:
    DGSSolver();
    ~DGSSolver() {};
    virtual void linearization();
    virtual double iteration(bool linearization = true) override;
    virtual double cost() const override;
    virtual void update_remote_poses(std::vector<double*> poses) override;
    virtual std::vector<ceres::Vector> get_last_local_states() override;

};

}