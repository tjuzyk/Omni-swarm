#pragma once


#include <ctime>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>

#define LOOP_BOW_THRES 0.015
// #define MATCH_INDEX_DIST 1
#define FAST_THRES (20.0f)

extern int JPG_QUALITY;

#define ACCEPT_LOOP_YAW (30) //ACCEPT MAX Yaw 
#define MAX_LOOP_DIS 5.0 //ACCEPT MAX DISTANCE, 2.0 for indoor flying

extern int INIT_MODE_MIN_LOOP_NUM; //Init mode we accepte this inlier number
extern int INIT_MODE_MIN_LOOP_NUM_LEVEL2; //Init mode we accepte this inlier number
extern int MIN_LOOP_NUM;

#define MAX_LOOP_DIS_LEVEL2 3.0 //ACCEPT MAX DISTANCE, 2.0 for indoor flying

#define DEG2RAD (0.01745277777777778)

#define ACCEPT_LOOP_YAW_RAD ACCEPT_LOOP_YAW*DEG2RAD

#define USE_DEEPNET

#define DEEP_DESC_SIZE 1024

#define SEARCH_NEAREST_NUM 5
#define ACCEPT_NONKEYFRAME_WAITSEC 5.0
#define INIT_ACCEPT_NONKEYFRAME_WAITSEC 1.0

extern double INNER_PRODUCT_THRES;
extern double INIT_MODE_PRODUCT_THRES;//INIT mode we can accept this inner product as similar
extern int MATCH_INDEX_DIST;

#define ORB_HAMMING_DISTANCE 40 //Max hamming
#define ORB_UV_DISTANCE 1.5 //UV distance bigger than mid*this will be removed

#define VISUALIZE_SCALE 2 //Scale for visuallize

#define CROP_WIDTH_THRES 0.05 //If movement bigger than this, crop some matches down

#define OUTLIER_XY_PRECENT_0 0.03 // This is given up match dx dy 
#define OUTLIER_XY_PRECENT_20 0.03 // This is given up match dx dy 
#define OUTLIER_XY_PRECENT_30 0.03 // This is given up match dx dy 
#define OUTLIER_XY_PRECENT_40 0.03 // This is given up match dx dy 

#define PNP_REPROJECT_ERROR 10.0
#define AVOID_GROUND_PRECENT 0.666 // This is for avoiding detect a lot feature on ground
// #define DEBUG_SHOW_IMAGE

#define ENABLE_OPTICAL_SEC_TRY_INIT 

extern int ACCEPT_MIN_3D_PTS;


#define RPERR_THRES 10*DEG2RAD

extern double DEPTH_NEAR_THRES;
extern double DEPTH_FAR_THRES;

#define FEATURE_DESC_SIZE 64

extern int MAX_DIRS;

#define ACCEPT_SP_MATCH_DISTANCE 0.7

extern int MIN_DIRECTION_LOOP;

extern int MIN_MATCH_PRE_DIR;

extern double TRIANGLE_THRES;

extern double DETECTOR_MATCH_THRES;

extern bool ENABLE_LK_LOOP_DETECTION;

extern bool USE_DEPTH;

extern bool IS_PC_REPLAY;

extern bool SEND_ALL_FEATURES;

extern bool LOWER_CAM_AS_MAIN;

extern bool OUTPUT_RAW_SUPERPOINT_DESC;

extern std::string OUTPUT_PATH;
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


template<typename T, typename B>
inline void reduceVector(std::vector<T> &v, std::vector<B> status)
{
    int j = 0;
    for (int i = 0; i < int(v.size()); i++)
        if (status[i])
            v[j++] = v[i];
    v.resize(j);
}

enum CameraConfig{
    STEREO_PINHOLE = 0,
    STEREO_FISHEYE = 1,
    PINHOLE_DEPTH = 2,
    FOURCORNER_FISHEYE = 3
};

