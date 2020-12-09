//
// Created by xuhao on 19-6-4.
//

#include "swarm_localization/swarm_localization_solver.hpp"


#include <iostream>
#include "glog/logging.h"
#include <eigen3/Eigen/Dense>
#include "ceres/ceres.h"
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <time.h>
#include <thread>
#include <unistd.h>
#include "swarm_localization/localiztion_costfunction.hpp"
#include <functional>
#include <swarm_localization/swarm_types.hpp>
#include <set>
#include <chrono>
#include <graphviz/cgraph.h>
#include "swarm_localization/localization_DA_init.hpp"

using namespace std::chrono;

// #define DEBUG_OUTPUT_POSES
// #define DEBUG_OUTPUT_LOOPS
// #define DEBUG_OUTPUT_COV
// #define DEBUG_OUTPUT_NEW_KF

#define SMALL_MOVEMENT_SPD 0.1
#define REPLACE_MIN_DURATION 0.1
// #define ENABLE_REPLACE
#define MAX_SOLVER_TIME 0.5

#define NOT_MOVING_THRES 0.02
#define NOT_MOVING_YAW 0.05

#define THRES_YAW_OBSER_XY 1.0

#define RAND_INIT_XY 5
#define RAND_INIT_Z 1

#define INIT_TRIAL 5

#define BEGIN_MIN_LOOP_DT 10.0

//For testing loop closure for single drone, use 1
#define MIN_DRONES_NUM 1
#define RE_ESTIMATE_SELF_POSES

#define SINGLE_DRONE_SFS_THRES 3

#define RANDOM_DELETE_KF 

#define ENABLE_DEPTH true


float VO_DRIFT_METER;
float VO_DRIFT_METER_Z;
float VO_ERROR_ANGLE;
float DISTANCE_MEASURE_ERROR;
float LOOP_COV_XY;
float LOOP_COV_Z;
float LOOP_YAWCOV;
float DETECTION_SPHERE_COV;
float DETECTION_INV_DEP_COV;

SwarmLocalizationSolver::SwarmLocalizationSolver(const swarm_localization_solver_params & _params) :
            params(_params), max_frame_number(_params.max_frame_number), min_frame_number(_params.min_frame_number),
            thread_num(_params.thread_num), acpt_cost(_params.acpt_cost),min_accept_keyframe_movement(_params.kf_movement),
            init_xy_movement(_params.init_xy_movement),init_z_movement(_params.init_z_movement),dense_frame_number(_params.dense_frame_number),
            cgraph_path(_params.cgraph_path),enable_cgraph_generation(_params.enable_cgraph_generation), 
            loop_outlier_threshold_pos(_params.loop_outlier_threshold_pos),
            loop_outlier_threshold_yaw(_params.loop_outlier_threshold_yaw),
            enable_detection(_params.enable_detection),
            enable_loop(_params.enable_loop),
            enable_distance(_params.enable_distance)
    {
    }


int SwarmLocalizationSolver::judge_is_key_frame(const SwarmFrame &sf) {
    auto _ids = sf.node_id_list;
    std::vector<int> ret(0);

    if (_ids.size() < MIN_DRONES_NUM)
        return 0;

    if (sf_sld_win.empty()) {
        for (auto _id : _ids) {
            node_kf_count[_id] = 1;
        }

        if (sf.has_node(self_id) && sf.has_odometry(self_id)) {
            return 1;
        } else {
            return 0;
        }
    }

    const SwarmFrame & last_sf = sf_sld_win.back();
    double dt = (sf.stamp - last_sf.stamp).toSec();

    if (!sf.has_node(self_id) || !sf.has_odometry(self_id)) {
        return 0;
    }

    const NodeFrame & self_nf = sf.id2nodeframe.at(self_id);
    Eigen::Vector3d _diff = sf.position(self_id) - last_sf.position(self_id);

    //TODO: make it set to if last dont's have some detection and this frame has, than keyframe
    if (_diff.norm() > min_accept_keyframe_movement) { //here shall be some one see him or he see someone
        ret.push_back(self_id);
        node_kf_count[self_id] += 1;
        ROS_INFO("SF %d is kf of %d: DIFF %3.2f", TSShort(sf.ts), self_id, _diff.norm());
        return 1;
    }

    for (auto _id : _ids) {
        if (all_nodes.find(_id) == all_nodes.end()) {
            return 1;
        }
    }

    return 0;
}

void SwarmLocalizationSolver::delete_frame_i(int i) {
    auto delete_sf = sf_sld_win[i];
    sf_sld_win.erase(sf_sld_win.begin() + i);
    if (i < sf_sld_win.size()) {
        auto & next_sf = sf_sld_win[i];
        for (auto it: next_sf.id2nodeframe) {
            auto &_id = it.first;
            auto &_node = it.second;
            if(delete_sf.has_node(_id) && delete_sf.has_odometry(_id)) {
                //Than make this cov bigger
                _node.position_cov_to_last = _node.position_cov_to_last + VO_DRIFT_XYZ;
                _node.yaw_cov_to_last = _node.yaw_cov_to_last + VO_ERROR_ANGLE;
            }
        }

    }
}

bool SwarmLocalizationSolver::is_frame_useful(unsigned int i) const {
    for (unsigned int id : sf_sld_win.at(i).node_id_list) {
        if (node_kf_count.at(id) < min_frame_number) {
            return true;
        }
    }
    return false;
}

void SwarmLocalizationSolver::process_frame_clear() {
    //Delete non keyframe first
    int _index = 0;

#ifdef RANDOM_DELETE_KF
    while (sf_sld_win.size() > max_frame_number) {
        _index = rand()%(max_frame_number-1);
        delete_frame_i(_index);
        ROS_INFO("Clear first frame from sld win, now size %ld", sf_sld_win.size());
    }
#else
    while (sf_sld_win.size() > max_frame_number) {
        delete_frame_i(_index);
        ROS_INFO("Clear first frame from sld win, now size %ld", sf_sld_win.size());
    }
#endif
}

void SwarmLocalizationSolver::random_init_pose(EstimatePoses &swarm_est_poses, EstimatePosesIDTS &est_poses_idts) {
    for (auto it : swarm_est_poses) {
        for (auto it2 : it.second) {
            if (it2.first != self_id) {
                double * p = it2.second;
                p[0] = rand_FloatRange(-RAND_INIT_XY, RAND_INIT_XY);
                p[1] = rand_FloatRange(-RAND_INIT_XY, RAND_INIT_XY);
                p[2] = rand_FloatRange(-RAND_INIT_Z, RAND_INIT_Z);
                p[3] = all_sf[it.first].id2nodeframe[it2.first].yaw();
            }
        }
    }
}

void SwarmLocalizationSolver::init_dynamic_nf_in_keyframe(int64_t ts, NodeFrame &_nf) {
    int _id = _nf.id;
    EstimatePoses & est_poses = est_poses_tsid;
    EstimatePosesIDTS & est_poses2 = est_poses_idts;
    auto _p = new double[4];
    if (_id != self_id || finish_init) {
        //Self should also init this way
        Pose est_last;
        if (last_kf_ts > 0 && est_poses_idts.find(_id) != est_poses_idts.end()) {
            //Use last solve relative res, e.g init with last

            int64_t last_ts_4node = est_poses_idts[_id].rbegin()->first;
            est_last = Pose(est_poses_tsid[last_ts_4node][_id], true);

            Pose last_vo = all_sf[last_ts_4node].id2nodeframe[_id].pose();
            Pose now_vo = _nf.pose();

            Pose dpose = Pose::DeltaPose(last_vo, now_vo, true);

            if ( dpose.pos().norm() < NOT_MOVING_THRES && fabs(dpose.yaw()) < NOT_MOVING_YAW ) {
                //NOT MOVING; Merging pose
                delete _p;
                _p = est_poses_tsid[last_ts_4node][_id];
            } else {
                Pose predict_now = est_last * dpose;
                predict_now.to_vector_xyzyaw(_p);
            }
            // ROS_INFO("Init ID %d at %d with predict value", _nf.id, TSShort(ts));
        } else {
            ROS_INFO("Init ID %d at %d with random value", _nf.id, TSShort(ts));
            est_last.set_pos(_nf.pose().pos() + rand_FloatRange_vec(-RAND_INIT_XY, RAND_INIT_XY));
            est_last.set_att(_nf.pose().att());
            est_last.to_vector_xyzyaw(_p);
        }
    } else {
        //Only not finish and self id use this
        Pose p = _nf.pose();
        p.to_vector_xyzyaw(_p);
    }

    if (est_poses.find(ts) == est_poses.end()) {
        est_poses[ts] = std::map<int, double*>();
    }

    if(est_poses2.find(_id) == est_poses2.end()) {
        est_poses2[_id] = std::map<int64_t, double*>();
    }
    est_poses[ts][_id] = _p;
    est_poses2[_id][ts] = _p;
}


void SwarmLocalizationSolver::init_static_nf_in_keyframe(int64_t ts, NodeFrame &_nf) {
    int _id = _nf.id;
    EstimatePoses & est_poses = est_poses_tsid;
    EstimatePosesIDTS & est_poses2 = est_poses_idts;
    double * _p = nullptr;
    if (last_kf_ts > 0 && est_poses2.find(_id) != est_poses2.end()) {
        _p = est_poses2[_id].begin()->second;
    } else {
        _p = new double[4];
        Pose _last;
        double noise = RAND_INIT_XY;
        _last.set_pos(_nf.pose().pos() + rand_FloatRange_vec(-noise, noise));
        _last.set_att(_nf.pose().att());
        _last.to_vector_xyzyaw(_p);
    }

    if (est_poses.find(ts) == est_poses.end()) {
        est_poses[ts] = std::map<int, double*>();
    }

    if(est_poses2.find(_id) == est_poses2.end()) {
        est_poses2[_id] = std::map<int64_t, double*>();
    }
    est_poses[ts][_id] = _p;
    est_poses2[_id][ts] = _p;
}

void SwarmLocalizationSolver::print_frame(const SwarmFrame & sf) const {
    if (!finish_init) {
        return;
    }
    
    printf("\n");
    ROS_INFO("------------------------------------\nKF %d details", TSShort(sf.ts));

    const SwarmFrame & last_sf = all_sf.at(last_kf_ts);

    for (auto it : sf.id2nodeframe) {
        auto id = it.first;
        auto _nf = it.second;
        printf("ID %d \n", id);
        if (est_poses_idts.at(id).find(last_kf_ts) == est_poses_idts.at(id).end() ) {
            ROS_INFO("Can't find id in last KF %d", TSShort(last_kf_ts));
            continue;
        }
        double* pose_last = est_poses_idts.at(id).at(last_kf_ts);
        if (!last_sf.has_node(id) || !last_sf.id2nodeframe.at(id).vo_available) 
            return;
        Pose pose_vo_last = last_sf .id2nodeframe.at(id).pose();
        int64_t ts =  sf.ts;
        double * pose = est_poses_idts.at(id).at(ts);
        auto pose_vo = sf.id2nodeframe.at(id).pose();
        auto poseest = Pose(pose, true);
        printf("POSVO        %3.4f %3.4f %3.4f YAW %5.4fdeg\n",
                pose_vo.pos().x(), pose_vo.pos().y(), pose_vo.pos().z(), pose_vo.yaw()*57.3);
        printf("POSEST     %3.4f %3.4f %3.4f YAW %5.4fdeg\n",
                poseest.pos().x(), poseest.pos().y(), poseest.pos().z(), pose_vo.yaw()*57.3);
        Pose DposeVO = Pose::DeltaPose(pose_vo_last, pose_vo, true);
        Pose DposeEST = Pose::DeltaPose(Pose(pose_last, true), Pose(pose, true), true);
        Pose ERRVOEST = Pose::DeltaPose(DposeVO, DposeEST, true);
        double ang_err = ERRVOEST.yaw()*1000;
        
        printf("ERRVOEST(mm)       %6.5f %6.5f %6.5f ANG  %3.2f\n",
                ERRVOEST.pos().x()*1000, ERRVOEST.pos().y()*1000, ERRVOEST.pos().z()*1000, ang_err);

        printf("DPOSVO         %6.5f %6.5f %3.4f YAW %5.4fdeg\n",
                DposeVO.pos().x(), DposeVO.pos().y(), DposeVO.pos().z(), DposeVO.yaw()*57.3);

        printf("DPOSEST        %6.5f %6.5f %3.4f YAW %5.4fdeg\n",
                DposeEST.pos().x(), DposeEST.pos().y(), DposeEST.pos().z(), DposeEST.yaw()*57.3);

        if (_nf.dis_map.size() > 0) {
            printf("DISTANCES ");
            for (auto itj : _nf.dis_map) {
                int _idj = itj.first;
                double dis = itj.second;
                if (sf.has_node(_idj) && sf.id2nodeframe.at(_idj).vo_available) {
                    if (est_poses_idts.find(_idj) == est_poses_idts.end() || est_poses_idts.at(_idj).find(ts) == est_poses_idts.at(_idj).end()) {
                        printf("Can't find %d at %d\n", _idj, TSShort(ts));
                        continue;
                    }

                    Pose posj_est(est_poses_idts.at(_idj).at(ts), true);
                    double est_dis = (posj_est.pos() - poseest.pos()).norm();
                    printf("ID %d DIS %4.2f EST %4.2f ",_idj, dis, est_dis);
                }
            }
            printf("\n");
        }

        printf("--------------------------------------------------------------------\n\n");
    }    
}

void SwarmLocalizationSolver::add_as_keyframe(const SwarmFrame &sf) {
    // if (sf_sld_win.size() > 0) {
        // last_kf_ts = sf_sld_win.back().ts;
    // }

    sf_sld_win.push_back(sf);
    all_sf[sf.ts] = sf;
    ROS_INFO("New keyframe %d found, size %ld/%d", TSShort(sf.ts), sf_sld_win.size(), max_frame_number);
    for (auto it : sf.id2nodeframe) {
        if (it.second.is_static) {
            ROS_INFO("Is static");
            this->init_static_nf_in_keyframe(sf.ts, it.second);
        } else {
            auto _nf = it.second;
            double _pose[4];
            _nf.pose().to_vector_xyzyaw(_pose);
            this->init_dynamic_nf_in_keyframe(sf.ts, it.second);
        }
    }

#ifdef DEBUG_OUTPUT_NEW_KF
    print_frame(sf);
#endif
    last_kf_ts = sf.ts;
    has_new_keyframe = true;
}

void SwarmLocalizationSolver::add_new_detection(const swarm_msgs::node_detected_xyzyaw & detected) {
    if (enable_detection) {
        all_detections.push_back(detected);
        has_new_keyframe = true;
    }
}

void SwarmLocalizationSolver::add_new_loop_connection(const swarm_msgs::LoopConnection & loop_con) {
    if (enable_loop) {
        all_loops.push_back(loop_con);
        has_new_keyframe = true;
    }
}


void SwarmLocalizationSolver::replace_last_kf(const SwarmFrame &sf) {
    delete_frame_i(sf_sld_win.size()-1);
    sf_sld_win.push_back(sf);
    all_sf[sf.ts] = sf;

    for (auto it : sf.id2nodeframe) {
        if (it.second.is_static) {
            ROS_INFO("Is static");
            this->init_static_nf_in_keyframe(sf.ts, it.second);
        } else {
            this->init_dynamic_nf_in_keyframe(sf.ts, it.second);
        }
    }
    last_kf_ts = sf.ts;
    has_new_keyframe = true;
}


void SwarmLocalizationSolver::add_new_swarm_frame(const SwarmFrame &sf) {
    process_frame_clear();

    auto _ids = sf.node_id_list;

    int is_kf = judge_is_key_frame(sf);
    if (is_kf == 1) {
        for (int _id : _ids) {
            all_nodes.insert(_id);
        }

        add_as_keyframe(sf);
        ROS_INFO("New kf found, sld win size %ld TS %d NFTS %d ID: [", sf_sld_win.size(),
            TSShort(sf_sld_win.back().ts),
            TSShort(sf_sld_win.back().id2nodeframe[self_id].ts)
        );
        for (int _id : _ids) {
            printf(" %d", _id);
        }
        printf("]\n");
    }

#ifdef ENABLE_REPLACE
    if (is_kf == 2) {
        replace_last_kf(sf);
        ROS_INFO("Replace last kf with TS %d",  TSShort(sf_sld_win.back().ts));
    }
#endif

    if (_ids.size() > drone_num) {
        //For here the drone num increase
        drone_num = _ids.size();
    }

    
}


bool SwarmLocalizationSolver::PredictNode(const NodeFrame & nf, Pose & _pose, Eigen::Matrix4d & cov) const {
    std::pair<Pose, Eigen::Matrix4d> ret; 
    int _id = nf.id;
    if (last_saved_est_kf_ts.size() > 0 && finish_init) {
        for (auto it = last_saved_est_kf_ts.rbegin(); it != last_saved_est_kf_ts.rend(); ++it ) { 
            int64_t _ts = *it;
            if(est_poses_tsid_saved.at(_ts).find(_id)!=est_poses_tsid_saved.at(_ts).end() ) {

                //Use last solve relative res, e.g init with last
                int _id = nf.id;
                Pose est_last = Pose(est_poses_tsid_saved.at(_ts).at(_id), true);
                Pose last_vo = all_sf.at(_ts).id2nodeframe.at(_id).pose();
                Pose now_vo = nf.pose();


                _pose = est_last * Pose::DeltaPose(last_vo, now_vo, true);
                cov = Eigen::Matrix4d::Zero();
                return true;
            }
        }
    }
    return false;
}


bool SwarmLocalizationSolver::NodeCooridnateOffset(int _id, Pose & _pose, Eigen::Matrix4d & cov) const {
    bool find_node = false;
    if (last_saved_est_kf_ts.size() > 0 && finish_init) {
        for (auto it = last_saved_est_kf_ts.rbegin(); it != last_saved_est_kf_ts.rend(); ++it ) { 
            int64_t _ts = *it;
            if(est_poses_tsid_saved.at(_ts).find(_id)!=est_poses_tsid_saved.at(_ts).end() ) {
                find_node = true;
                Pose PBA = Pose(est_poses_tsid_saved.at(_ts).at(_id), true);

                Pose PBB = all_sf.at(_ts).id2nodeframe.at(_id).pose();
                
                PBA.set_yaw_only();
                PBB.set_yaw_only();
                
                _pose = Pose(PBA.to_isometry() * PBB.to_isometry().inverse());
                
#ifdef COMPUTE_COV
                if (_id != self_id){
                    auto cov = est_cov_tsid.at(last_saved_est_kf_ts).at(_id);
                    cov.block<3,3>(0,0) = PBA.to_isometry().rotation()*cov.block<3,3>(0,0);
                    ret.second = cov;
                } else {
                    ret.second = Eigen::Matrix4d::Zero();
                }
#else
                cov = Eigen::Matrix4d::Zero();
#endif
                return true;
            }
        }
    }
    return false;
}


SwarmFrameState SwarmLocalizationSolver::PredictSwarm(const SwarmFrame &sf) const {
    SwarmFrameState sfs;
    if(!finish_init) {
        ROS_WARN("Predict swarm poses failed: SwarmLocalizationSolver not inited\n");
        return sfs;
    }
    
    for (auto it : sf.id2nodeframe) {
        int _id = it.first;

        NodeFrame & nf = it.second;
        Pose pose, pose1;
        Eigen::Matrix4d cov, cov1;
        auto ret = this->PredictNode(nf, pose, cov);
        if (ret) {
            sfs.node_poses[_id] = pose;
            sfs.node_covs[_id] = cov;
        }
        //Give node velocity predict here
        sfs.node_vels[_id] = Eigen::Vector3d(0, 0, 0);
        ret = this->NodeCooridnateOffset(nf.id, pose1, cov1);
        if (ret) {
            sfs.base_coor_poses[_id] = pose1;
            sfs.base_coor_covs[_id] = cov1;

        }
    }

    return sfs;
}


bool SwarmLocalizationSolver::solve_with_multiple_init(int max_number) {

    double cost = acpt_cost;
    bool cost_updated = false;

    ROS_WARN("Try to use %d random init to solve expect cost %f", max_number, cost);
    //Need to rewrite here to enable multiple trial of input!!!
    EstimatePoses _est_poses_best;// = est_poses_tsid;
    EstimatePoses & _est_poses = est_poses_tsid;
    EstimatePosesIDTS & _est_poses_idts = est_poses_idts;
 
    for (int i = 0; i < max_number; i++) {
        ROS_WARN("%d time of init trial", i);
        random_init_pose(_est_poses,  _est_poses_idts);
        double c = solve_once(_est_poses,  _est_poses_idts,  true);

        if (c < cost) {
            ROS_INFO("Got better cost %f", c);
            cost_updated = true;
            cost_now = cost = c;
            // return true;
            for (auto it : est_poses_tsid) {
                auto ts = it.first;
                if (_est_poses_best.find(ts) != _est_poses_best.end()) {
                    for (auto it2: _est_poses_best[ts]) {
                        delete [] (_est_poses_best[ts][it2.first]);
                    }
                }
                _est_poses_best[ts] = std::map<int, double*>();
                for (auto it2: it.second) {
                    auto _id = it2.first;
                    double * _p = new double[4];
                    _est_poses_best[ts][_id] = _p;
                    memcpy(_p, _est_poses[ts][_id], 4*sizeof(double));
                }
            }
        }
    }

    if (cost_updated) {
        for (auto it : est_poses_tsid) {
            auto ts = it.first;
            for (auto it2: it.second) {
                auto _id = it2.first;
                memcpy(_est_poses[ts][_id], _est_poses_best[ts][_id], 4*sizeof(double));
                delete [] (_est_poses_best[ts][_id]);
            }
        }
    }

    return cost_updated;
}


std::pair<Eigen::Vector3d, Eigen::Vector3d> SwarmLocalizationSolver::boundingbox_sldwin(int _id) const {
    double xmax=-1000, xmin = 1000, ymax = -1000, ymin = 1000, zmax = -1000, zmin = 1000;
    for (const SwarmFrame & _sf : sf_sld_win ) {
        if (_sf.has_node(_id) && _sf.id2nodeframe.at(_id).vo_available ) {
            Vector3d pos = _sf.id2nodeframe.at(_id).position();
            if (pos.x() > xmax) {
                xmax = pos.x();
            }
            if (pos.y() > ymax) {
                ymax = pos.y();
            }
            if (pos.z() > zmax) {
                zmax = pos.z();
            }

            if (pos.x() < xmin) {
                xmin = pos.x();
            }
            if (pos.y() < ymin) {
                ymin = pos.y();
            }
            if (pos.z() < zmin) {
                zmin = pos.z();
            }
        }
    }

    return std::make_pair(Eigen::Vector3d(xmin, ymin, zmin),
        Eigen::Vector3d(xmax, ymax, zmax));
}

        
double SwarmLocalizationSolver::solve() {
    if (self_id < 0 || sf_sld_win.size() < min_frame_number)
        return -1;

    if (!has_new_keyframe)
        return cost_now;

    estimate_observability();

    // if (!finish_init) {
    //     //Use da initer to initial the system
    //     LocalizationDAInit DAIniter(sf_sld_win, params.DA_TRI_accept_thres);
    //     std::map<int, int> mapper;
    //     bool success = DAIniter.try_data_association(mapper);
    //     if (success) {
    //         ROS_INFO("Success initial system with visual data association");
    //         for (auto it : mapper) {
    //             ROS_INFO("UNIDENTIFIED %d ASSOCIATION %d", it.first, it.second);
    //         }
    //     } else {
    //         ROS_INFO("Could not initail system with visual data association");
    //     }
    // }


    if (!finish_init) {
        //Init procedure
        if (enable_to_init) {
            ROS_INFO("No init before, try to init");
            finish_init = solve_with_multiple_init(INIT_TRIAL);
            if (finish_init) {
                last_drone_num = drone_num;
                ROS_INFO("Finish init\n");
            }
        } else {
            ROS_WARN("BOUNDING BOX too small; Pending more movement");
            return -1;
        }
       
    } else if (has_new_keyframe) {
        // ROS_INFO("New keyframe, solving....%d good_loop %ld", enable_cgraph_generation, good_2drone_measurements.size());
        if (enable_cgraph_generation) {
            generate_cgraph();
        }
        cost_now = solve_once(this->est_poses_tsid, this->est_poses_idts, true);
    }

    if (cost_now > acpt_cost) {
        finish_init = false;
    }

    if (finish_init) {
        sync_est_poses(this->est_poses_tsid);
    }
    printf("\n\n");
    return cost_now;
}

void  SwarmLocalizationSolver::sync_est_poses(const EstimatePoses &_est_poses_tsid) {
    ROS_INFO("Sync poses to saved while init successful");
    int64_t last_ts = sf_sld_win.back().ts;
    for (const SwarmFrame & sf : sf_sld_win) {
        //Only update param in sf to saved
        for (auto it : sf.id2nodeframe) {
            int _id = it.first;
            const NodeFrame _nf = it.second;
            if (est_poses_tsid_saved.find(sf.ts) == est_poses_tsid_saved.end()) {
                est_poses_tsid_saved[sf.ts] = std::map<int,double*>();
            }
            if (est_poses_idts_saved.find(_id) == est_poses_idts_saved.end()) {
                est_poses_idts_saved[_id] = std::map<int64_t,double*>();
            }

            if (est_poses_tsid_saved[sf.ts].find(_id) == est_poses_tsid_saved[sf.ts].end()) {
                est_poses_tsid_saved[sf.ts][_id] = new double[4];
            }

            if (est_poses_idts_saved[_id].find(sf.ts) == est_poses_idts_saved[_id].end()) {
                est_poses_idts_saved[_id][sf.ts] = new double[4];
            }  
            
            if (_est_poses_tsid.find(sf.ts) !=_est_poses_tsid.end() &&
                _est_poses_tsid.at(sf.ts).find(_id) != _est_poses_tsid.at(sf.ts).end()
            ) {
                last_ts = sf.ts;
                memcpy(est_poses_tsid_saved[sf.ts][_id], _est_poses_tsid.at(sf.ts).at(_id), 4*sizeof(double));
                memcpy(est_poses_idts_saved[_id][sf.ts], _est_poses_tsid.at(sf.ts).at(_id), 4*sizeof(double));
            } 
        }
    }

    last_saved_est_kf_ts.push_back(last_ts);
}

unsigned int SwarmLocalizationSolver::sliding_window_size() const {
    return sf_sld_win.size();
}

CostFunction *
SwarmLocalizationSolver::_setup_cost_function_by_sf(const SwarmFrame &sf, std::map<int, int> id2poseindex, bool is_lastest_frame, int & res_num) const {
    //Here we will only send
    std::map<int, double> yaw_init;
    for (const auto & it : sf.id2nodeframe) {
//            auto _id = it.first;
        const NodeFrame &_nf = it.second;
        //First we come to distance error
        if (_nf.frame_available) {
            yaw_init[_nf.id] = est_poses_tsid.at(_nf.ts).at(_nf.id)[3];
        }
    }
    SwarmFrameError * sferror = new SwarmFrameError(sf, id2poseindex, yaw_observability, yaw_init);
    res_num = sferror->residual_count();
    auto cost_function  = new SFErrorCost(sferror);
    
    for (auto it : id2poseindex) {
        int _id = it.first;
        if (!yaw_observability.at(_id)) {
            cost_function->AddParameterBlock(3);
        } else {
            cost_function->AddParameterBlock(4);
        }
    }

    // assert(res_num > 0 &&"Set cost function with SF has 0 res num");
    if (res_num > 0) {
        cost_function->SetNumResiduals(res_num);
        return cost_function;
    } else {
        return nullptr;
    }
}

    
CostFunction *
SwarmLocalizationSolver::_setup_cost_function_by_loop(const std::vector<Swarm::GeneralMeasurement2Drones*> & loops, IDTSIndex  _id_ts_poseindex) const {
    auto sle = new SwarmLoopError(loops, _id_ts_poseindex);
    auto cost_function = new LoopCost(sle);
    int res_num = sle->residual_count();
    cost_function->SetNumResiduals(res_num);
    std::set<int> all_index;
    for (auto it : _id_ts_poseindex) {
        for (auto it2 : it.second) {
            if (all_index.find(it2.second) == all_index.end() ) {
                cost_function->AddParameterBlock(4);
                all_index.insert(it2.second);
            }
        }
    }

    return cost_function;
}
    
void SwarmLocalizationSolver::setup_problem_with_loops(const EstimatePosesIDTS & est_poses_idts, Problem &problem) const {

    std::vector<double*> pose_state; // For involved poses
    std::set<double*> added_poses;

    if (good_2drone_measurements.size() == 0) {
        // ROS_INFO("No loop; Return");
        return;
    }
    // ROS_INFO("Find %ld good loops", good_2drone_measurements.size());
    IDTSIndex  _id_ts_poseindex;

    for (auto loc : good_2drone_measurements) {
         if (_id_ts_poseindex.find(loc->id_a) == _id_ts_poseindex.end()) {
            _id_ts_poseindex[loc->id_a] = std::map<int64_t, int>();
        }

        if (_id_ts_poseindex.find(loc->id_b) == _id_ts_poseindex.end()) {
            _id_ts_poseindex[loc->id_b] = std::map<int64_t, int>();
        }

        double * posea = est_poses_idts.at(loc->id_a).at(loc->ts_a);
        if (added_poses.find(posea) == added_poses.end()) {
            pose_state.push_back(posea);
            _id_ts_poseindex[loc->id_a][loc->ts_a] = pose_state.size() - 1;
            added_poses.insert(posea);
        }

        double * poseb = est_poses_idts.at(loc->id_b).at(loc->ts_b);
        if (added_poses.find(poseb) == added_poses.end()) {
            pose_state.push_back(poseb);
            _id_ts_poseindex[loc->id_b][loc->ts_b] = pose_state.size() - 1;
            added_poses.insert(poseb);
        }
    }

    CostFunction * cost = _setup_cost_function_by_loop(good_2drone_measurements, _id_ts_poseindex);
    ceres::LossFunction *loss_function;
    loss_function = new ceres::HuberLoss(0.1);
    //loss_function = new ceres::HuberLoss(0.5);
    problem.AddResidualBlock(cost, loss_function, pose_state);
}
    


void SwarmLocalizationSolver::setup_problem_with_sferror(const EstimatePoses & swarm_est_poses, Problem& problem, const SwarmFrame& sf, TSIDArray& param_indexs, bool is_lastest_frame) const {
    //TODO: Deal with static object in this function!!!
    std::vector<double*> pose_state;
    std::map<int, int> id2poseindex;
    std::vector<int> _id_list;
    int64_t ts = sf.ts;
    for(auto it : sf.id2nodeframe) {
        int _id = it.first;
        // ROS_INFO("Add TS %d ID %d", TSShort(ts), _id);
        pose_state.push_back(swarm_est_poses.at(ts).at(_id));
        _id_list.push_back(_id);
        id2poseindex[_id] = pose_state.size() - 1;
        param_indexs.push_back(std::pair<int64_t, int>(ts, _id));
    }
    int res_num = 0;
    auto loss_function = new ceres::HuberLoss(0.1);
    CostFunction * cost = _setup_cost_function_by_sf(sf, id2poseindex, is_lastest_frame, res_num);

    if (cost != nullptr) {
        problem.AddResidualBlock(cost, loss_function, pose_state);
        if (finish_init) {
            /*
            printf("SF Evaluate ERROR ts %d", TSShort(ts));
            double * res = new double[res_num];
            cost->Evaluate(pose_state.data(), res, nullptr);
            for (int i = 0; i < res_num; i++) {
                printf(" %f ", res[i]);
            }
            printf("\n");*/
        }
    } else {
        for (unsigned int i = 0; i < pose_state.size(); i ++) {
            double * _state = pose_state[i];
            int _id = _id_list[i];
            
            if (!yaw_observability.at(_id)) {
                problem.AddParameterBlock(_state, 3);
            } else {
                problem.AddParameterBlock(_state, 4);
            }
        }
    }

}

CostFunction *
SwarmLocalizationSolver::_setup_cost_function_by_nf_win(std::vector<NodeFrame> &nf_win, const std::map<int64_t, int> & ts2poseindex, bool is_self) const {
    int _id = nf_win[0].id;
    std::vector<double> yaw_init;
    for (NodeFrame & _nf : nf_win) {
        yaw_init.push_back(est_poses_tsid.at(_nf.ts).at(_nf.id)[3]);
    }
    auto she = new SwarmHorizonError(nf_win, ts2poseindex, yaw_observability.at(_id), yaw_init);
    auto cost_function = new HorizonCost(she);
    int res_num = she->residual_count();

    int poses_num = nf_win.size();

    for (int i =0;i < poses_num; i ++) {
        if (!yaw_observability.at(_id)) {
            cost_function->AddParameterBlock(3);
        } else {
            cost_function->AddParameterBlock(4);
        }
    }
    if (res_num == 0) {
        ROS_WARN("Set cost function with NF has 0 res num; NF id %d WIN %ld", nf_win[0].id, nf_win.size());
        // exit(-1);
        return nullptr;
    } else {
        // ROS_INFO("nf_win of %d res_num %d", _id, res_num);
    }

    cost_function->SetNumResiduals(res_num);
    return cost_function;
}

void SwarmLocalizationSolver::setup_problem_with_sfherror(const EstimatePosesIDTS & est_poses_idts, Problem& problem, int _id) const {
    auto nfs = est_poses_idts.at(_id);

 
    std::vector<NodeFrame> nf_win;
    std::vector<double*> pose_win;
    std::map<int64_t, int> ts2poseindex;

    for (const SwarmFrame & sf : sf_sld_win) {
        int64_t ts = sf.ts;
        if (nfs.find(ts) != nfs.end()) {
            auto _p = nfs[ts];
            if (pose_win.size() < 1 || pose_win[pose_win.size()-1] != _p) {
                pose_win.push_back(nfs[ts]);
                const NodeFrame & _nf = all_sf.at(ts).id2nodeframe.at(_id);
                if(_nf.is_static) {
                    return;
                }
                nf_win.push_back(_nf);
                ts2poseindex[ts] = nf_win.size() - 1;
            }

            // ROS_INFO("Add TS %d ID %d", TSShort(ts), _id);
            
        } 

    }

    if (_id == self_id) {
        problem.SetParameterBlockConstant(pose_win[0]);
    }

    if (nfs.size() < 2) {
        ROS_INFO("Frame nums for id %d is to small:%ld", _id, nfs.size());
        return;
    }

    CostFunction * cf = _setup_cost_function_by_nf_win(nf_win, ts2poseindex, _id==self_id);
    if (cf != nullptr) {
        problem.AddResidualBlock(cf , nullptr, pose_win);
    } else {
        ROS_WARN("Emptry swarm fram horizon error");
    }
}

bool SwarmLocalizationSolver::NFnotMoving(const NodeFrame & _nf1, const NodeFrame & _nf2) const {
    Eigen::Vector3d _diff = _nf1.position() - _nf2.position();
    //TODO: make it set to if last dont's have some detection and this frame has, than keyframe
    if (_diff.norm() > NOT_MOVING_THRES) {
        return false;
    }
    return true;
}

void SwarmLocalizationSolver::cutting_edges() {

    int distance_count = 0;
    int total_distance_count = 0;
    int total_detection_count = all_detections.size();

    SwarmFrame & sf0 = sf_sld_win[0];
    for (auto & it : sf0.id2nodeframe) {
        auto & _nf = it.second;
        _nf.enabled_distance.clear();
        for (auto it_dis : _nf.dis_map) {
            int _id2 = it_dis.first;
            _nf.enabled_distance[_id2] = true;
            distance_count += 1;
            total_distance_count += 1;
        }
    }

    for (unsigned int i = 1; i < sf_sld_win.size(); i++) {
        SwarmFrame & sf = sf_sld_win[i];
        SwarmFrame & last_sf = sf_sld_win[i - 1];
        std::set<int> moved_nodes; //Mark the node not moved from last sf

        for (auto & it : sf.id2nodeframe) {
            auto _nf = it.second;
            auto _id = it.first;
            if (!last_sf.has_node(_id) || 
                !NFnotMoving(last_sf.id2nodeframe[_id], _nf)
                ) {
                moved_nodes.insert(_id);
            }
        }
        // Now we have all moved node; Let's begin with edging enabling
        for (auto & it : sf.id2nodeframe) {
            NodeFrame & _nf = it.second;
            auto _id = it.first;
            _nf.enabled_distance.clear();
            for (auto it_dis : _nf.dis_map) {
                int _id2 = it_dis.first;
                _nf.enabled_distance[_id2] = false;
                total_distance_count += 1;
                if ((moved_nodes.find(_id) != moved_nodes.end() ||
                    moved_nodes.find(_id2) != moved_nodes.end())) {                    
                    if( sf.has_node(_id2) && 
                        (sf.id2nodeframe[_id2].enabled_distance.find(_id) == sf.id2nodeframe[_id2].enabled_distance.end() || !sf.id2nodeframe[_id2].enabled_distance[_id])) {
                        //ROS_INFO("Merging distanc %3.2f and %3.2f to %3.2f", 
                        //    _nf.dis_map[_id2],
                        //    sf.id2nodeframe[_id2].dis_map[_id],
                        //    (_nf.dis_map[_id2] + sf.id2nodeframe[_id2].dis_map[_id])/2.0
                        //);
                        _nf.dis_map[_id2] = (_nf.dis_map[_id2] + sf.id2nodeframe[_id2].dis_map[_id])/2.0;
                        _nf.enabled_distance[_id2] = true;
                        distance_count += 1;
                    }
                }
            }
        }
    }

    ROS_INFO("Edge Optimized DIS %d(%d) All Det and LOOPS %ld", distance_count, total_distance_count, total_detection_count, good_2drone_measurements.size());
    /*
    for (auto & sf : sf_sld_win) {
        for (auto & it : sf.id2nodeframe) {
            auto _nf = it.second;
            auto _id = it.first;
            ROS_WARN("TS %d ID %d ENABLED %ld DISMAP %ld\n", TSShort(_nf.ts), _nf.id, _nf.dis_map.size(), _nf.enabled_distance.size());
        }
    }*/
}

std::set<int> SwarmLocalizationSolver::loop_observable_set(const std::map<int, std::set<int>> & loop_edges) const {
    std::set<int> observerable_set;
    observerable_set.insert(self_id);
    
    if (all_nodes.size() > 1) {
        //Check min tree here
        std::vector<int> queue;
        queue.push_back(self_id);
        while(true) {
            if (queue.empty()) {
                break;
            }

            int _id = queue[0];
            queue.erase(queue.begin());
            
            //Send nodes connect to _id to obs set and queue
            if (loop_edges.find(_id) != loop_edges.end()) {
                for (auto _c_id : loop_edges.at(_id)) {
                    if (observerable_set.find(_c_id) == observerable_set.end()) {
                        //Not in set yet, add to set and queue
                        observerable_set.insert(_c_id);
                        queue.push_back(_c_id);
                    }
                }
            }
        }
    }
    
    printf("Loop observable nodes is: ");
    for (auto _id : observerable_set) {
        printf("%d, ", _id);
    }
    printf("\n");
    return observerable_set;
}

void SwarmLocalizationSolver::estimate_observability() {
    yaw_observability.clear();
    for (auto p : good_2drone_measurements) {
        delete p;
    }
    good_2drone_measurements.clear();
    good_2drone_measurements = find_available_loops_detections(loop_edges);

    // ROS_INFO("GOOD LOOPS NUM %ld", good_2drone_measurements.size());
    for (int _id : all_nodes) {
        //Can't deal with machines power on later than movement
        pos_observability[_id] = false;
        yaw_observability[_id] = false;
    }


    auto bbx = boundingbox_sldwin(self_id);
    auto min = bbx.first;
    auto max = bbx.second;
        if (max.x() - min.x() > init_xy_movement &&
            max.y() - min.y() > init_xy_movement &&
            max.z() - min.z() > init_z_movement
    ) {
        // If bbx is big enough
        enable_to_init = true;
        for (int _id : all_nodes) {
            //Can't deal with machines power on later than movement
            pos_observability[_id] = true;
        }

        ROS_INFO("Solve with enough movement");

    }

    std::set<int> _loop_observable_set = loop_observable_set(loop_edges);
    
    if (sf_sld_win.size() > SINGLE_DRONE_SFS_THRES && all_nodes.size() == 1) {
        //Has 3 KF and 1 big
        enable_to_init = true;
        ROS_INFO("Solve with single drone");
    }

    if (!enable_to_init) {

        if (_loop_observable_set.size() < all_nodes.size() || all_nodes.size() < 2) {
            ROS_INFO("Can't initial with loop only, the OB/ALL size %ld/%ld. Swarm Frame Sliding Window:", 
                _loop_observable_set.size(),
                all_nodes.size()
            );

            for (auto & sf : sf_sld_win) {
                sf.print();
                printf("\n");
            }

        } else {
            ROS_INFO("Solve with loop");
            enable_to_init = true;
        }
    }

    for (int _id : _loop_observable_set) {
        pos_observability[_id] = true;
        yaw_observability[_id] = true;
    }

    printf("YAW observability: ");
    
    for (int _id: all_nodes) {
        auto bbx = boundingbox_sldwin(_id);
        auto min = bbx.first;
        auto max = bbx.second;

        if (max.x() - min.x() > THRES_YAW_OBSER_XY || max.y() - min.y() > THRES_YAW_OBSER_XY) {
            yaw_observability[_id] = true;
        }
        printf("%d: %s ", _id, yaw_observability[_id]?"true":"false");
    }

    printf("\n");
}

bool SwarmLocalizationSolver::find_node_frame_for_measurement_2drones(const Swarm::GeneralMeasurement2Drones * loc, int & _index_a, int &_index_b) const {
    ros::Time tsa = loc->stamp_a;
    ros::Time tsb = loc->stamp_b;
    
    int _ida = loc->id_a;
    int _idb = loc->id_b;
    double min_ts_err_a = 10000;
    double min_ts_err_b = 10000;

        for (unsigned int i = 0; i < sf_sld_win.size(); i++ ) {
        //Find suitable timestamp for tsa
        //If the first frame is older than tsa, than useless

        if (sf_sld_win[i].has_node(_ida) && fabs((sf_sld_win.at(i).id2nodeframe.at(_ida).stamp - tsa).toSec()) < min_ts_err_a) {
            min_ts_err_a = fabs((sf_sld_win.at(i).id2nodeframe.at(_ida).stamp - tsa).toSec());
            _index_a = i;
        }

        if (sf_sld_win[i].has_node(_idb) && fabs((sf_sld_win[i].id2nodeframe.at(_idb).stamp - tsb).toSec()) < min_ts_err_b) {
            min_ts_err_b = fabs((sf_sld_win.at(i).id2nodeframe.at(_idb).stamp - tsb).toSec());
            _index_b = i;
        }
    }

    if (_index_a < 0 || _index_b < 0) {
        ROS_WARN("loop_from_src_loop_connection. Loop [TS%d]%d->[TS%d]%d; SF0 TS [%d] DT %f not found in L1116", TSShort(tsa.toNSec()), _ida, TSShort(tsb.toNSec()), _idb, TSShort(sf_sld_win[0].ts), (sf_sld_win[0].stamp - tsa).toSec());
        return false;
    }
    return true;
}


bool SwarmLocalizationSolver::detection_from_src_node_detection(const swarm_msgs::node_detected_xyzyaw & _det, Swarm::DroneDetection & det_ret, double & dt_err, double & dpos) const {
    ros::Time ts = _det.header.stamp;
    
    int _ida = _det.self_drone_id;
    int _idb = _det.remote_drone_id;
    int _index_a = -1;
    int _index_b = -1;
    double min_ts_err_a = 10000;
    double min_ts_err_b = 10000;

    //Give up if first timestamp is bigger than 1 sec than tsa
    if (sf_sld_win.empty()) {
        ROS_WARN("Can't find loop No sld win");
        return false;
    }

    // if((sf_sld_win[0].stamp - tsa).toSec() > BEGIN_MIN_LOOP_DT) {
    //     ROS_WARN("loop_from_src_loop_connection. Loop [TS%d]%d->[TS%d]%d; SF0 TS [%d] DT %f not found in L1164", TSShort(tsa.toNSec()), _ida, TSShort(tsb.toNSec()), _idb, TSShort(sf_sld_win[0].ts), (sf_sld_win[0].stamp - tsa).toSec());
    //     return false;
    // }

    det_ret = Swarm::DroneDetection(_det, ENABLE_DEPTH);

    bool success = find_node_frame_for_measurement_2drones(&det_ret, _index_a, _index_b);
    if (!success) {
        ROS_WARN("Detection find failed");
        return false;
    } else {
    }
   
    const NodeFrame & _nf_a = sf_sld_win.at(_index_a).id2nodeframe.at(_ida);
    const NodeFrame & _nf_b = sf_sld_win.at(_index_b).id2nodeframe.at(_idb);



    Pose dpose_self_a = Pose::DeltaPose(_nf_a.self_pose, det_ret.self_pose_a, true); //2->0
    Pose dpose_self_b = Pose::DeltaPose(det_ret.self_pose_b, _nf_b.self_pose, true); //1->3

    det_ret.dpose_self_a = dpose_self_a;
    det_ret.dpose_self_b = dpose_self_b;

    det_ret.ts_a = _nf_a.ts;
    det_ret.ts_b = _nf_b.ts;
    det_ret.self_pose_a = _nf_a.pose();
    det_ret.self_pose_b = _nf_b.pose();

#ifdef DEBUG_OUTPUT_DETS

    printf("SELF POSE A");
    _nf_a.self_pose.print();
    printf("SELF POSE B");
    _nf_b.self_pose.print();
    printf("SELF POSE A1");
    det_ret.self_pose_a.print();
    printf("SELF POSE B1");
    det_ret.self_pose_b.print();

    printf("DPOSE A");
    dpose_self_a.print();
    printf("DPOSE B");
    dpose_self_b.print();

    printf("Det [TS%d]%d->[TS%d]%d; DTS a %4.3fms b %4.3fms LOOP:", TSShort(tsa.toNSec()), _ida, TSShort(tsb.toNSec()), 
        _idb, min_ts_err_a*1000, min_ts_err_b*1000);
    new_loop.print();
#endif
    dt_err = min_ts_err_a + min_ts_err_b;
    dpos = dpose_self_a.pos().norm() +  dpose_self_b.pos().norm();
    return true;
}


bool SwarmLocalizationSolver::loop_from_src_loop_connection(const swarm_msgs::LoopConnection & _loc, Swarm::LoopConnection & loc_ret, double & dt_err, double & dpos) const{
    ros::Time tsa = _loc.ts_a;
    ros::Time tsb = _loc.ts_b;
    
    int _ida = _loc.id_a;
    int _idb = _loc.id_b;
    int _index_a = -1;
    int _index_b = -1;
    double min_ts_err_a = 10000;
    double min_ts_err_b = 10000;

    //Give up if first timestamp is bigger than 1 sec than tsa
    if (sf_sld_win.empty()) {
        ROS_WARN("Can't find loop No sld win");
        return false;
    }

    // if((sf_sld_win[0].stamp - tsa).toSec() > BEGIN_MIN_LOOP_DT) {
    //     ROS_WARN("loop_from_src_loop_connection. Loop [TS%d]%d->[TS%d]%d; SF0 TS [%d] DT %f not found in L1164", TSShort(tsa.toNSec()), _ida, TSShort(tsb.toNSec()), _idb, TSShort(sf_sld_win[0].ts), (sf_sld_win[0].stamp - tsa).toSec());
    //     return false;
    // }

    loc_ret = Swarm::LoopConnection(_loc);

    bool success = find_node_frame_for_measurement_2drones(&loc_ret, _index_a, _index_b);
    if (!success) {
        return false;
    }
   
    const NodeFrame & _nf_a = sf_sld_win.at(_index_a).id2nodeframe.at(_ida);
    const NodeFrame & _nf_b = sf_sld_win.at(_index_b).id2nodeframe.at(_idb);



    Pose dpose_self_a = Pose::DeltaPose(_nf_a.self_pose, loc_ret.self_pose_a, true); //2->0
    Pose dpose_self_b = Pose::DeltaPose(loc_ret.self_pose_b, _nf_b.self_pose, true); //1->3

    Pose new_loop = dpose_self_a * loc_ret.relative_pose * dpose_self_b;

    loc_ret.ts_a = _nf_a.ts;
    loc_ret.ts_b = _nf_b.ts;
    loc_ret.self_pose_a = _nf_a.pose();
    loc_ret.self_pose_b = _nf_b.pose();
    loc_ret.relative_pose = new_loop;
    
    if (finish_init) {
        const double * posea = est_poses_tsid.at(_nf_a.ts).at(_ida);
        const double * poseb = est_poses_tsid.at(_nf_b.ts).at(_idb);
        auto posea_est = Pose(posea, true);
        auto poseb_est = Pose(poseb, true);
        Pose dpose_est = Pose::DeltaPose(posea_est, poseb_est, true);
        Pose dpose_err = Pose::DeltaPose(dpose_est, new_loop, true);
        if (dpose_err.pos().norm()>loop_outlier_threshold_pos || fabs(dpose_err.yaw()) > loop_outlier_threshold_yaw) {
            ROS_WARN("Loop Error %d(%d)->%d(%d) P%3.2f Y%3.2f. Give up this loop", 
                _ida, TSShort(loc_ret.ts_a), _idb, TSShort(loc_ret.ts_b), dpose_err.pos().norm(), dpose_err.yaw()*57.3);
            return false;
        }
    }

    dt_err = min_ts_err_a + min_ts_err_b;

    dpos = dpose_self_a.pos().norm() +  dpose_self_b.pos().norm();

    return true;
}

std::vector<Swarm::LoopConnection*> average_same_loop(std::vector<Swarm::LoopConnection> good_2drone_measurements) {
    //tuple 
    //    int64_t ts_a, int64_t ts_b, int id_a int id_b;
    std::map<Swarm::GeneralMeasurement2DronesKey, std::vector<Swarm::LoopConnection>> loop_sets;
    std::vector<Swarm::LoopConnection*> ret;
    for (auto & loop : good_2drone_measurements) {
        GeneralMeasurement2DronesKey key = loop.key();
        if (loop_sets.find(key) == loop_sets.end()) {
            loop_sets[key] = std::vector<Swarm::LoopConnection>();
        }

        loop_sets[key].push_back(loop);
    }
    
    good_2drone_measurements.clear();
    
    for (auto & it : loop_sets) {
        auto & loop_vec = it.second;
        Eigen::Vector3d pos_sum(0, 0, 0);
        double yaw_sum = 0;
        for (auto loop : loop_vec) {
            pos_sum = pos_sum + loop.relative_pose.pos();
            yaw_sum = yaw_sum + loop.relative_pose.yaw();
        }

        auto loop = new Swarm::LoopConnection(loop_vec[0]);
        
        loop->relative_pose = Swarm::Pose(pos_sum/loop_vec.size(), yaw_sum/loop_vec.size());
        loop->avg_count = loop_vec.size();
        ret.push_back(loop);
    }

    return ret;
}

std::vector<GeneralMeasurement2Drones*> SwarmLocalizationSolver::find_available_loops_detections(std::map<int, std::set<int>> & loop_edges) const {
    loop_edges.clear();
    std::vector<Swarm::LoopConnection> good_loops;
    std::vector<Swarm::DroneDetection> good_detections;
    std::vector<GeneralMeasurement2Drones*> ret;

    for (auto _loc : all_loops) {
        Swarm::LoopConnection loc_ret;
        double dt_err = 0;
        double dpos;
        if(loop_from_src_loop_connection(_loc, loc_ret, dt_err, dpos)) {
            // ROS_INFO("Loop [%d]%d -> [%d]%d [%3.2f, %3.2f, %3.2f] %f Pa [%3.2f, %3.2f, %3.2f] %f Pb [%3.2f, %3.2f, %3.2f] %f ", TSShort(loc_ret.ts_a), loc_ret.id_a,  TSShort(loc_ret.ts_b), loc_ret.id_b,
            //     loc_ret.relative_pose.pos().x(), loc_ret.relative_pose.pos().y(), loc_ret.relative_pose.pos().z(),  loc_ret.relative_pose.yaw(),
            //     loc_ret.self_pose_a.pos().x(), loc_ret.self_pose_a.pos().y(), loc_ret.self_pose_a.pos().z(),  loc_ret.self_pose_a.yaw(),
            //     loc_ret.self_pose_b.pos().x(), loc_ret.self_pose_b.pos().y(), loc_ret.self_pose_b.pos().z(),  loc_ret.self_pose_b.yaw());
            good_loops.push_back(loc_ret);
            if (loop_edges.find(loc_ret.id_a) == loop_edges.end()) {
                loop_edges[loc_ret.id_a] = std::set<int>();
            }
            if (loop_edges.find(loc_ret.id_b) == loop_edges.end()) {
                loop_edges[loc_ret.id_b] = std::set<int>();
            }
            loop_edges[loc_ret.id_a].insert(loc_ret.id_b);
            loop_edges[loc_ret.id_b].insert(loc_ret.id_a);
        }
    }

    auto ret_loops = average_same_loop(good_loops);
    for (auto p : ret_loops) {
        ret.push_back(static_cast<Swarm::GeneralMeasurement2Drones *>(p));
    }

    for (auto _det : all_detections) {
        Swarm::DroneDetection det_ret;
        double dt_err = 0;
        double dpos;
        if(detection_from_src_node_detection(_det, det_ret, dt_err, dpos)) {
            // ROS_INFO("Loop [%d]%d -> [%d]%d [%3.2f, %3.2f, %3.2f] %f Pa [%3.2f, %3.2f, %3.2f] %f Pb [%3.2f, %3.2f, %3.2f] %f ", TSShort(loc_ret.ts_a), loc_ret.id_a,  TSShort(loc_ret.ts_b), loc_ret.id_b,
            //     loc_ret.relative_pose.pos().x(), loc_ret.relative_pose.pos().y(), loc_ret.relative_pose.pos().z(),  loc_ret.relative_pose.yaw(),
            //     loc_ret.self_pose_a.pos().x(), loc_ret.self_pose_a.pos().y(), loc_ret.self_pose_a.pos().z(),  loc_ret.self_pose_a.yaw(),
            //     loc_ret.self_pose_b.pos().x(), loc_ret.self_pose_b.pos().y(), loc_ret.self_pose_b.pos().z(),  loc_ret.self_pose_b.yaw());
            good_detections.push_back(det_ret);
            if (loop_edges.find(det_ret.id_a) == loop_edges.end()) {
                loop_edges[det_ret.id_a] = std::set<int>();
            }
            if (loop_edges.find(det_ret.id_b) == loop_edges.end()) {
                loop_edges[det_ret.id_b] = std::set<int>();
            }
            loop_edges[det_ret.id_a].insert(det_ret.id_b);
            loop_edges[det_ret.id_b].insert(det_ret.id_a);
        }
    }

    // auto ret_loops = average_same_loop(good_loops);
    for (auto p : good_detections) {
        auto ptr = new Swarm::DroneDetection(p);
        ret.push_back(static_cast<Swarm::GeneralMeasurement2Drones *>(ptr));
    }

    ROS_INFO("All loops %ld, all detections %ld good_2drone_measurements %ld averaged loop %ld good_detections %ld",
        all_loops.size(), all_detections.size(),
        ret.size(), ret.size(), good_detections.size());
    return ret;
}

double SwarmLocalizationSolver::solve_once(EstimatePoses & swarm_est_poses, EstimatePosesIDTS & est_poses_idts, bool report) {

    ros::Time t1 = ros::Time::now();
    Problem problem;

//        if (solve_count % 10 == 0)
    printf("SOLVE COUNT %d Trying to solve size %d, TS %ld, good_loop and detections %ld\n", solve_count, sliding_window_size(), swarm_est_poses.size(), good_2drone_measurements.size());
    has_new_keyframe = false;
    std::vector<std::pair<int64_t, int>> param_indexs;
    cutting_edges();

    for (unsigned int i = 0; i < sf_sld_win.size(); i++ ) {
        // ROS_INFO()
        this->setup_problem_with_sferror(swarm_est_poses, problem, sf_sld_win[i], param_indexs, i==sf_sld_win.size()-1);
    }

    int num_res_blks_sf = problem.NumResidualBlocks();
    int num_res_sf = problem.NumResiduals();
    ROS_INFO("SF residual blocks %d residual nums %d", num_res_blks_sf, num_res_sf);
    num_res_blks_sf = problem.NumResidualBlocks();

    for (int _id: all_nodes) {
        this->setup_problem_with_sfherror(est_poses_idts, problem, _id);       
    }

    ROS_INFO("SFH residual blocks %d residual nums %d", problem.NumResidualBlocks() - num_res_blks_sf, problem.NumResiduals() - num_res_sf);

    num_res_sf = problem.NumResiduals();
    setup_problem_with_loops(est_poses_idts, problem);

    ROS_INFO("Loop residual blocks %d residual nums %d", problem.NumResidualBlocks() - num_res_blks_sf, problem.NumResiduals() - num_res_sf);
    num_res_sf = problem.NumResiduals();


    ceres::Solver::Options options;

    //SPARSE NORMAL DOGLEG 12.5ms
    //SPARSE NORMAL 21
    //DENSE NORM DOGLEG 49.31ms
    options.max_num_iterations = 1000;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    if (finish_init) {
        options.max_solver_time_in_seconds = MAX_SOLVER_TIME;
        options.max_num_iterations = 1000;
    }
    
    options.num_threads = thread_num;
    Solver::Summary summary;

    options.trust_region_strategy_type = ceres::DOGLEG;
    
    ros::Time t2 = ros::Time::now();

    ceres::Solve(options, &problem, &summary);


    if (summary.termination_type == ceres::TerminationType::FAILURE) {
        ROS_ERROR("Ceres critical failure. Exiting...");
        exit(-1);
    }

    double equv_cost = summary.final_cost / sliding_window_size();

    if (num_res_sf > 1) {
        equv_cost = equv_cost / num_res_sf;
    }

    equv_cost = sqrt(equv_cost)/ERROR_NORMLIZED;
    if (!report) {
        return equv_cost;
    }

    std::cout << "\nSize:" << sliding_window_size() << "\n" << summary.BriefReport() << " Equv cost : "
              << equv_cost << " Time : " << summary.total_time_in_seconds * 1000 << "ms\n";
    std::cout << summary.message << std::endl;

#ifdef DEBUG_OUTPUT_POSES
    //if (finish_init) 
    {

        for (auto it : est_poses_idts) {
            auto id = it.first;
            ROS_INFO("\n\nID %d ", id);
            double* pose_last = nullptr;
            Pose pose_vo_last;
            for(auto it2 : it.second) {
                auto ts = it2.first;
                // double * pose = it2.second;
                // auto ts = sf_sld_win.back().ts;
                if (est_poses_tsid[ts].find(id) == est_poses_tsid[ts].end()) {
                    continue;
                }

                double * pose = est_poses_tsid[ts][id];
                auto pose_vo = all_sf[ts].id2nodeframe[id].pose();
                auto poseest = Pose(pose, true);
                ROS_INFO("TS %d POS %3.4f %3.4f %3.4f YAW %5.4fdeg", TSShort(ts), pose[0], pose[1], pose[2], wrap_angle(pose[3])*57.3);
                ROS_INFO("POSVO        %3.4f %3.4f %3.4f YAW %5.4fdeg",
                        pose_vo.pos().x(), pose_vo.pos().y(), pose_vo.pos().z(), pose_vo.yaw()*57.3);
                ROS_INFO("POSVOEST     %3.4f %3.4f %3.4f YAW %5.4fdeg",
                        poseest.pos().x(), poseest.pos().y(), poseest.pos().z(), pose_vo.yaw()*57.3);
                if (pose_last!=nullptr) {
                    Pose DposeVO = Pose::DeltaPose(pose_vo_last, pose_vo, true);
                    Pose DposeEST = Pose::DeltaPose(Pose(pose_last, true), Pose(pose, true), true);
                    Pose ERRVOEST = Pose::DeltaPose(DposeVO, DposeEST, true);
                    double ang_err = ERRVOEST.yaw()/ VO_ERROR_ANGLE;
                    
                    ROS_WARN("ERRVOEST       %6.5f %6.5f %6.5f ANG  %3.2f",
                            ERRVOEST.pos().x()/VO_DRIFT_METER, ERRVOEST.pos().y()/VO_DRIFT_METER, ERRVOEST.pos().z()/VO_DRIFT_METER, ang_err);

                    ROS_INFO("DPOSVO         %6.5f %6.5f %3.4f YAW %5.4fdeg",
                            DposeVO.pos().x(), DposeVO.pos().y(), DposeVO.pos().z(), DposeVO.yaw()*57.3);

                    ROS_INFO("DPOSEST        %6.5f %6.5f %3.4f YAW %5.4fdeg\n\n",
                            DposeEST.pos().x(), DposeEST.pos().y(), DposeEST.pos().z(), DposeEST.yaw()*57.3);
                }

                printf("DISTANCES ");
                for (auto itj : all_sf[ts].id2nodeframe[id].dis_map) {
                    int _idj = itj.first;
                    double dis = itj.second;
                    if (all_sf[ts].id2nodeframe[_idj].vo_available) {
                        Pose posj_vo = all_sf[ts].id2nodeframe[_idj].pose();
                        Pose posj_est(est_poses_idts[_idj][ts], true);
                        double vo_dis = (posj_vo.pos() - pose_vo.pos()).norm();
                        double est_dis = (posj_est.pos() - Pose(pose, true).pos()).norm();
                        printf(" DIS %4.2f VO %4.2f EST %4.2f ", dis, vo_dis, est_dis);
                    }
                }

                printf("\n");


                pose_last = pose;
                pose_vo_last = pose_vo;
            }
        }

    }
#endif

    solve_time_count += summary.total_time_in_seconds;
    solve_count++;

    ROS_INFO("AVG Solve %3.2fms Dt1 %3.2f ms TOTAL %3.2fms\n", solve_time_count *1000 / solve_count, 
    (t2-t1).toSec()*1000, (ros::Time::now() - t1).toSec()*1000);

    return equv_cost;
}


void SwarmLocalizationSolver::generate_cgraph() {
    auto start = high_resolution_clock::now();
    Agraph_t *g;
    g = agopen("G", Agdirected, NULL);
    char node_name[100] = {0};
    char edgename[100] = {0};

    agattr(g,AGRAPH,"shape","box");
    agattr(g,AGRAPH,"style","filled");
    agattr(g,AGRAPH,"label","Pose Graphs");
    agattr(g,AGNODE,"style","filled");
    agattr(g,AGEDGE,"color","black");
    agattr(g,AGEDGE,"label","residual");

    std::map<int64_t, std::map<int, Agnode_t*>> AGNodes;
    
    for (auto & sf : sf_sld_win) {
        sprintf(node_name, "cluster_%d", TSShort(sf.ts));
        auto sub_graph = agsubg(g, node_name, 1);
        //	style=filled;
        //   color=lightgrey;
        // label = "process #1";
        sprintf(node_name, "SwarmFrame %d", TSShort(sf.ts));
        agattrsym (sub_graph, "label");
        agset (sub_graph, "label", node_name);


        AGNodes[sf.ts] = std::map<int, Agnode_t*>();
        for (auto & _it : sf.id2nodeframe) {
            int _id = _it.first;
            NodeFrame & _nf = _it.second;
            sprintf(node_name, "Node%d_%d", _id, TSShort(sf.ts));
            auto ag_node = agnode(sub_graph, node_name, 1);
            AGNodes[sf.ts][_id] = ag_node;
        }
    }
    std::vector<double*> pose_win;

    //Add all vio residuals
    for (auto _id : all_nodes) {
        // ROS_INFO("Gen edge for node %d", _id);
        auto nfs = est_poses_idts.at(_id);
        Agnode_t * node1 = nullptr;

        for (const SwarmFrame & sf : sf_sld_win) {
            int64_t ts = sf.ts;
            // ROS_INFO("Gen edge for node %d", _id);
            if (nfs.find(ts) != nfs.end()) {
                // ROS_INFO("NFS can find ts %d", TSShort(ts));
                auto _p = nfs[ts];
                if (pose_win.size() < 1 || pose_win[pose_win.size()-1] != _p) {
                    pose_win.push_back(nfs[ts]);
                    if (node1 == nullptr) {
                        node1 = AGNodes[ts][_id];
                    } else {
                        auto node2 = AGNodes[ts][_id];
                        auto edge = agedge(g, node1, node2, "VIO",1);
                        agattrsym (edge, "label");
                        Swarm::Pose dp = Swarm::Pose::DeltaPose(
                            Swarm::Pose(pose_win[pose_win.size()-2], true), 
                            Swarm::Pose(pose_win.back(), true)
                        );
                        sprintf(edgename, "VIO:DP [%3.2f,%3.2f,%3.2f] DY %4.3f", dp.pos().x(), dp.pos().y(), dp.pos().z(),
                            dp.yaw()*57.3);
                        agset(edge, "label", edgename);

                        // printf("Adding edge...\n");
                        node1 = node2;
                    }
                }
            } 
        }
    }

    //
    int count = 0;
    for (auto & _loop: good_2drone_measurements) {
        if (_loop->meaturement_type == Swarm::GeneralMeasurement2Drones::Loop)
        {
            auto loop = static_cast<Swarm::LoopConnection * >(_loop);
            
            sprintf(edgename, "loop(%d->%d dt %4.1fms); DP [%3.2f,%3.2f,%3.2f] DY %4.3f", 
                loop->id_a, loop->id_b, (loop->ts_b - loop->ts_a)/1000000.0,
                loop->relative_pose.pos().x(),
                loop->relative_pose.pos().y(),
                loop->relative_pose.pos().z(),
                loop->relative_pose.yaw()*57.3
            );

            // sprintf(edgename, "loop(%d->%d T:%4.1fs)", loop->id_a, loop->id_b, (loop->ts_b - loop->ts_a)/1e9);

            char loopname[10] = {0};
            sprintf(loopname, "Loop %d", count);
            auto edge = agedge(g, AGNodes[loop->ts_a][loop->id_a], AGNodes[loop->ts_b][loop->id_b], loopname, 1);
            agattrsym (edge, "label");
            agattrsym (edge, "color");
            agset(edge, "label", edgename);
            agset(edge, "color", "orange");

            count += 1;
        } else {
            auto det = static_cast<Swarm::DroneDetection * >(_loop);
            sprintf(edgename, "Detection(%d->%d)",
                det->id_a, det->id_b);

            char loopname[10] = {0};
            sprintf(loopname, "Det %d", count);
            auto edge = agedge(g, AGNodes[det->ts_a][det->id_a], AGNodes[det->ts_b][det->id_b], loopname, 1);
            agattrsym (edge, "label");
            agattrsym (edge, "color");
            agset(edge, "label", edgename);
            agset(edge, "color", "orange");

            count += 1;
        }
    }
    FILE * f = fopen(cgraph_path.c_str(), "w");
    agwrite(g,f);
    agclose(g);
    fclose(f);
    ROS_INFO("Generated cgraph to %s", cgraph_path.c_str());
    double dt = duration_cast<microseconds>(high_resolution_clock::now() - start).count()/1000.0;

    ROS_INFO("Generate cgraph cost %4.3fms\n", dt);

}
