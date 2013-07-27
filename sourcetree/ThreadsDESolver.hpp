
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

#ifndef THREADSDESOLVER_H_
#define THREADSDESOLVER_H_

#include <thread>
#include <random>
#include <mutex>
#include <condition_variable>
#include <algorithm>
 
#include "BaseDE.hpp"

namespace pdebc {

enum class WorkType {
	SOLVE_GENERATION,
	GET_BEST_CANDIDATE
};

template <class POP_TYPE, int POP_DIM, int POP_SIZE, class ERROR_TYPE>
struct ThreadsDESolver {

	const int kID_;
	BaseDE<POP_TYPE,POP_DIM,POP_SIZE,ERROR_TYPE>* base_de_;

	Matrix<POP_TYPE, POP_SIZE, POP_DIM> population_;

	ThreadsDESolver(const int id,
		BaseDE<POP_TYPE,POP_DIM,POP_SIZE,ERROR_TYPE>* base_de)
		: kID_{id}, base_de_{base_de}, finish_{false}, pending_work_{false},
			work_ready_{false} {
	}
	~ThreadsDESolver() {
	}

	void start() {
		using MyThreadsDESolver = pdebc::ThreadsDESolver<POP_TYPE,POP_DIM,POP_SIZE,ERROR_TYPE>;
		thread_ = std::thread(&MyThreadsDESolver::run,this);
	}

	void join() {
		std::unique_lock<std::mutex> lock(mutex_);
		pending_work_ = true;
		finish_ = true;
		cond_.notify_one();
		lock.unlock();
		thread_.join();
	}

	void solveOneGeneration() {
		using namespace std;
		unique_lock<mutex> lock(mutex_);
		pending_work_ = true;
		work_ready_ = false;
		work_type_ = WorkType::SOLVE_GENERATION;
		cond_.notify_one();
	}

	void solveBestCandidate() {
		using namespace std;
		unique_lock<mutex> lock(mutex_);
		pending_work_ = true;
		work_ready_ = false;
		work_type_ = WorkType::GET_BEST_CANDIDATE;
		cond_.notify_one();
	}

	std::tuple<ERROR_TYPE,std::array<POP_TYPE,POP_DIM>> getBestCandidate() const {
		return best_candidate_;
	}

	void waitWork() {
		using namespace std;
		unique_lock<mutex> lock(work_ready_lock_);
		work_ready_cond_.wait(lock, [this]() {return this->work_ready_;});
	}

private:
	std::function<double()> random_cr_;
	std::function<uint32_t()> random_trials_;
	std::function<uint32_t()> random_j_;

	Matrix<POP_TYPE, 3, POP_DIM> pop_trials_; // Used in "mutation"
	std::array<POP_TYPE, POP_DIM> pop_candidate_;
	std::array<ERROR_TYPE, POP_SIZE> pop_errors_;

	std::tuple<ERROR_TYPE,std::array<POP_TYPE,POP_DIM>> best_candidate_;

	// Threads Flow Control
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cond_;
	bool pending_work_;
	bool finish_;
	bool work_ready_;
	WorkType work_type_;
	std::mutex work_ready_lock_;
	std::condition_variable work_ready_cond_;

	void run() {

		using namespace std;
		{ // this scope will be called only once
			// Initialize random_cr_
			mt19937 emt;
			uniform_real_distribution<double> ud(0.0, 1.0);
			random_cr_ = bind(ud, emt);
			// Initialize random_trials_
			mt19937 emt2;
			uniform_int_distribution<uint32_t> ui2(0, POP_SIZE-1);
			random_trials_ = bind(ui2, emt2);

			// Initialize random_j_
			mt19937 emt3;
			uniform_int_distribution<uint32_t> ui3(0, POP_DIM-1);
			random_j_ = bind(ui3, emt3);

			generatePopulation();
			calcGenerationError();
		}

		int work{0};
		while (!finish_) {
			// non-busy wait for more work
			unique_lock<mutex> lock(mutex_);
			cond_.wait(lock, [this]() {return this->pending_work_;});
			lock.unlock();

			if (finish_) {
				continue;
			}


			if (work_type_ == WorkType::SOLVE_GENERATION) {
				for (uint32_t i = 0; i < POP_SIZE; ++i) {
					mutation(i);
					select(i);
				}
			} else if (work_type_ == WorkType::GET_BEST_CANDIDATE) {
				auto e = std::min_element(pop_errors_.begin(), pop_errors_.end(),
				base_de_->callback_error_evaluation_);
				auto min = std::distance(pop_errors_.begin(), e);

				std::array<POP_TYPE,POP_DIM> r;
				for (int d = 0; d < POP_DIM; d++) {
					r[d] = population_[min][d];
				}

				best_candidate_ = std::tuple<ERROR_TYPE,std::array<POP_TYPE,POP_DIM>>{pop_errors_[min],r};
			}


			// Lets tell everyone we are DONE! <sigh>
			///   Need a full cycle to call it done!
			pending_work_ = false;
			lock_guard<mutex> work_read_lock(work_ready_lock_);
			work_ready_ = true;
			work_ready_cond_.notify_one();
		}
	}

	void generatePopulation() {
		for (uint32_t i = 0; i < POP_SIZE; ++i) {
			for (int d = 0; d < POP_DIM; ++d) {
				population_[i][d] = base_de_->callback_population_generator_();
			}
		}
	}

	void calcGenerationError() {
		for (uint32_t i = 0; i < POP_SIZE; ++i) {
			for (int d = 0; d < POP_DIM; ++d) {
				pop_candidate_[d] = population_[i][d];
			}
			pop_errors_[i] = base_de_->callback_calc_error_(pop_candidate_);
		}
	}

	void mutation(const uint32_t actual_index) {
		int j = random_j_();

		const uint32_t it0 = random_trials_();
		uint32_t it1 = random_trials_();
		while (it1 == it0) {
			it1 = random_trials_();
		}
		uint32_t it2 = random_trials_();
		while (it2 == it1 || it2 == it0) {
			it2 = random_trials_();
		}

		for (int d = 0; d < POP_DIM; d++) {
			pop_trials_[0][d] = population_[it0][d];
			pop_trials_[1][d] = population_[it1][d];
			pop_trials_[2][d] = population_[it2][d];
		}

		pop_candidate_[j] = pop_trials_[0][j] + base_de_->kF_ * (pop_trials_[1][j] - pop_trials_[2][j]);
		j = (j + 1) % POP_DIM;

		for (int k = 1; k < POP_DIM; ++k) {
			if (random_cr_() <= base_de_->kCR_) {
				pop_candidate_[j] = pop_trials_[0][j] + base_de_->kF_ * (pop_trials_[1][j] - pop_trials_[2][j]);
		    } else {
		      	pop_candidate_[j] = population_[actual_index][j];
		    }
		    j = (j + 1) % POP_DIM;
	    }
	}
	

	void select(const uint32_t actual_index) {
		ERROR_TYPE error_new = base_de_->callback_calc_error_(pop_candidate_);

		if (base_de_->callback_error_evaluation_(error_new, pop_errors_[actual_index])) {
			for (int d = 0; d < POP_DIM; d++) {
				population_[actual_index][d] = pop_candidate_[d];
			}
			pop_errors_[actual_index] = error_new;
		}
	}
};

};

 #endif /* THREADSDESOLVER_H_ */