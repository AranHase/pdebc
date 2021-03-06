/*
 Copyright 2012 Allan Yoshio Hasegawa

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 -----------------------------------------------------------------------------
 */

#ifndef THREADSDE_H_
#define THREADSDE_H_
#include <limits>
#include <functional>
#include <tuple>
#include <algorithm>
#include <chrono>

#include "BaseDE.hpp"
#include "ThreadsDESolver.hpp"

namespace pdebc {

//! Multi thread implementation of the Differential Evolution algorithm.
/*!
	\tparam POP_TYPE Population data type (usually 'double')
	\tparam POP_DIM Population dimensions (usually 2D or 3D)
	\tparam ERROR_TYPE Error type (usually 'double')
*/
template <class POP_TYPE, int POP_DIM, class ERROR_TYPE>
struct ThreadsDE : public BaseDE<POP_TYPE, POP_DIM, ERROR_TYPE> {

	const uint32_t kNProcess_; ///< Number of threads.
	const double kMigrationPhi_; ///< Chances of migration.
	const uint32_t kPopSize_; ///< Population size.

	/*!
		\param n_process Number of threads to use.
		\param migration_phi Chances of migration. Determines the probability
			of a population entity moving to the population of another thread.
			This values should be between [0,1].

		\param POP_SIZE Population size. Note that each thread will keep
			( ThreadsDE::kPopSize_ / ThreadsDE::kNProcess_ ) entities locally. So, it is
			advisable to pass a number divisible by ThreadsDE::kNProcess_.

		\param CR Mutation rate. Determines the chances
			of a mutation happening. This value must be 
			between [0,1].
		\param F Mutation weight. Determines how much
			the mutation impacts each trials. This value
			should be between [0,1].
		\param callback_population_generator Function used to generate each
			entity of the population. It must return a POP_TYPE type and use no
			parameters.
		\param callback_calc_error Function used to calculate the error with a single
			member of the population. It must return a ERROR_TYPE type and takes an
			array containg a single population entity as input parameter.
		\param callback_error_evaluation Fuction used to compare two ERROR_TYPE. It
			must return a bool. In case of true, the population from the first ERROR_TYPE
			will be picked as best candidate. Try to figure out what happens in case of false xD.
	*/
	ThreadsDE(const uint32_t n_process, const double migration_phi,
		const uint32_t POP_SIZE, const double CR, const double F,
		const std::function<POP_TYPE()>&& callback_population_generator,
		const std::function<ERROR_TYPE(const std::array<POP_TYPE,POP_DIM>&)>&& callback_calc_error,
		const std::function<bool(const ERROR_TYPE&,const ERROR_TYPE&)>&& callback_error_evaluation) :
			kNProcess_{n_process}, kMigrationPhi_{migration_phi},kPopSize_{POP_SIZE},
			BaseDE<POP_TYPE, POP_DIM, ERROR_TYPE>(
				CR, F,
				std::move(callback_population_generator),
				std::move(callback_calc_error),
				std::move(callback_error_evaluation)) {

		// Initialize random functions for the
		// migration step...
		using namespace std;
		mt19937 emt(random_device{}());
		uniform_real_distribution<double> ud(0.0, 1.0);
		random_phi_ = bind(ud, emt);

		mt19937 emt2(random_device{}());
		uniform_int_distribution<uint32_t> ui2(0, (kPopSize_/kNProcess_)-1);
		random_migration_index_ = bind(ui2, emt2);

		// Initialize each solver...
  		using MyThreadsDESolver = pdebc::ThreadsDESolver<POP_TYPE,POP_DIM,ERROR_TYPE>;
		for (int k = 0; k < kNProcess_; k++) {
			auto solver = shared_ptr<MyThreadsDESolver>(new MyThreadsDESolver(k,kPopSize_/kNProcess_,this));
			solvers_.push_back(solver);
		}
	}

	~ThreadsDE() {
  		solvers_.clear();
	}

	//! Solves one generation.
	/*!
		This is a blocking operation.
	*/
	void solveOneGeneration() {
		for (auto& s : solvers_) {
			s->solveOneGeneration();
		}
		for (auto& s : solvers_) {
			s->waitWork();
		}
		migration();
	}

	void solveNGenerations(const uint32_t N) {
		for (uint32_t g = 0; g < N; ++g) {
			solveOneGeneration();
		}
	}

	/*!
		This operation has an O(N) complexity, where N is the population size, but
		the work will be divided by ThreadsDE::kNProcess_ threads.
	*/
	std::tuple<ERROR_TYPE,std::array<POP_TYPE,POP_DIM>> getBestCandidate() {
		using namespace std;

		for (auto& s : solvers_) {
			s->solveBestCandidate();
		}
		solvers_[0]->waitWork();
		double min_error = get<0>(solvers_[0]->getBestCandidate());
		array<POP_TYPE,POP_DIM> min_error_pos = get<1>(solvers_[0]->getBestCandidate());;
		
		for (auto& s : solvers_) {
			s->waitWork();
			auto bc = s->getBestCandidate();
			if (get<0>(bc) < min_error) {
				min_error = get<0>(bc);
				min_error_pos = get<1>(bc);
			}
		}

		return std::tuple<ERROR_TYPE,std::array<POP_TYPE,POP_DIM>>{min_error,min_error_pos};
	}

private:
	std::function<double()> random_phi_;
	std::function<uint32_t()> random_migration_index_;
	std::vector<std::shared_ptr<ThreadsDESolver<POP_TYPE,POP_DIM,ERROR_TYPE>>> solvers_;

	// new step for the parallel solution ;)
	void migration() {
		using namespace std;

		for (auto& s : solvers_) {
			s->solveBestCandidate();
		}

		for (int i = 0; i < solvers_.size(); ++i) {
			if (random_phi_() < kMigrationPhi_) {
				solvers_[i]->waitWork();
				auto bc = get<1>(solvers_[i]->getBestCandidate());
				const uint32_t mi = random_migration_index_();
				solvers_[(i+1)%solvers_.size()]->population_[mi] = bc;
			}
		}
	}
};

} // namespace

#endif /* THREADSDE_H_ */
