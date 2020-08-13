/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   Frame.h
 * @brief  Class describing a pair of stereo images
 * @author Antoni Rosinol, Luca Carlone
 */

#include "kimera-vio/frontend/StereoFrame.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/core/core.hpp>

namespace VIO {

StereoFrame::StereoFrame(const FrameId& id,
                         const Timestamp& timestamp,
                         const Frame& left_frame,
                         const Frame& right_frame)
    : id_(id),
      timestamp_(timestamp),
      // TODO(Toni): remove these copies
      left_frame_(left_frame),
      right_frame_(right_frame),
      is_keyframe_(false),
      is_rectified_(false),
      left_img_rectified_(),
      right_img_rectified_() {
  CHECK_EQ(id_, left_frame_.id_);
  CHECK_EQ(id_, right_frame_.id_);
  CHECK_EQ(timestamp_, left_frame_.timestamp_);
  CHECK_EQ(timestamp_, right_frame_.timestamp_);
}

void StereoFrame::setRectifiedImages(const cv::Mat& left_rectified_img,
                                     const cv::Mat& right_rectified_img) {
  left_img_rectified_ = left_rectified_img;
  right_img_rectified_ = right_rectified_img;
}

void StereoFrame::checkStereoFrame() const {
  const size_t nrLeftKeypoints = left_frame_.keypoints_.size();
  CHECK_EQ(left_frame_.scores_.size(), nrLeftKeypoints)
      << "checkStereoFrame: left_frame_.scores.size()";
  CHECK_EQ(right_frame_.keypoints_.size(), nrLeftKeypoints)
      << "checkStereoFrame: right_frame_.keypoints_.size()";
  CHECK_EQ(keypoints_3d_.size(), nrLeftKeypoints)
      << "checkStereoFrame: keypoints_3d_.size()";
  CHECK_EQ(left_keypoints_rectified_.size(), nrLeftKeypoints)
      << "checkStereoFrame: left_keypoints_rectified_.size()";
  CHECK_EQ(right_keypoints_rectified_.size(), nrLeftKeypoints)
      << "checkStereoFrame: right_keypoints_rectified_.size()";

  for (size_t i = 0u; i < nrLeftKeypoints; i++) {
    if (right_keypoints_rectified_[i].first == KeypointStatus::VALID) {
      CHECK_LE(fabs(right_keypoints_rectified_[i].second.y -
                    left_keypoints_rectified_[i].second.y),
               3)
          << "checkStereoFrame: rectified keypoints have different y "
          << right_keypoints_rectified_[i].second.y << " vs. "
          << left_keypoints_rectified_[i].second.y;
    }

    if (right_keypoints_rectified_[i].first == KeypointStatus::VALID) {
      CHECK_NE(fabs(right_frame_.keypoints_[i].x) +
                   fabs(right_frame_.keypoints_[i].y),
               0)
          << "checkStereoFrame: right_frame_.keypoints_[i] is zero.";
      // Also: cannot have zero depth.
      CHECK_GT(keypoints_3d_[i](2), 0)
          << "checkStereoFrame: keypoints_3d_[i] has nonpositive "
             "for valid point: "
          << keypoints_3d_[i](2) << '\n'
          << "left_frame_.keypoints_[i] " << left_frame_.keypoints_[i] << '\n'
          << "right_frame_.keypoints_[i] " << right_frame_.keypoints_[i] << '\n'
          << '\n'
          << "right_keypoints_rectified_[i] st "
          << to_underlying(right_keypoints_rectified_[i].first)
          << '\n'
          << "right_keypoints_rectified_[i] kp "
          << right_keypoints_rectified_[i].second;
    } else {
      CHECK_LE(keypoints_3d_[i](2), 0)
          << "checkStereoFrame: keypoints_3d_[i] has positive "
             "for nonvalid point: "
          << keypoints_3d_[i](2);
    }
  }
}

void StereoFrame::checkStatusRightKeypoints(
    DebugTrackerInfo* debug_info) const {
  CHECK_NOTNULL(debug_info);
  debug_info->nrValidRKP_ = 0;
  debug_info->nrNoLeftRectRKP_ = 0;
  debug_info->nrNoRightRectRKP_ = 0;
  debug_info->nrNoDepthRKP_ = 0;
  debug_info->nrFailedArunRKP_ = 0;
  for (const StatusKeypointCV& right_keypoint : right_keypoints_rectified_) {
    KeypointStatus right_keypoint_status = right_keypoint.first;
    switch (right_keypoint_status) {
      case KeypointStatus::VALID: {
        debug_info->nrValidRKP_++;
        break;
      }
      case KeypointStatus::NO_LEFT_RECT: {
        debug_info->nrNoLeftRectRKP_++;
        break;
      }
      case KeypointStatus::NO_RIGHT_RECT: {
        debug_info->nrNoRightRectRKP_++;
        break;
      }
      case KeypointStatus::NO_DEPTH: {
        debug_info->nrNoDepthRKP_++;
        break;
      }
      case KeypointStatus::FAILED_ARUN: {
        debug_info->nrFailedArunRKP_++;
        break;
      }
      default: {
        LOG(FATAL) << "Unknown keypoint status.";
        break;
      }
    }
  }
}

void StereoFrame::setIsKeyframe(bool is_kf) {
  is_keyframe_ = is_kf;
  left_frame_.isKeyframe_ = is_kf;
  right_frame_.isKeyframe_ = is_kf;
}

std::vector<double> StereoFrame::getDepthFromRectifiedMatches(
    StatusKeypointsCV& left_keypoints_rectified,
    StatusKeypointsCV& right_keypoints_rectified,
    const double& fx,
    const double& baseline,
    const StereoMatchingParams& stereo_matching_params) const {
  // depth = fx * baseline / disparity (should be fx = focal * sensorsize)
  double fx_b = fx * baseline;

  std::vector<double> depths;
  CHECK_EQ(left_keypoints_rectified.size(), right_keypoints_rectified.size())
      << "getDepthFromRectifiedMatches: size mismatch!";

  int nrValidDepths = 0;
  // disparity = left_px.x - right_px.x, hence we check: right_px.x < left_px.x
  for (size_t i = 0; i < left_keypoints_rectified.size(); i++) {
    if (left_keypoints_rectified[i].first == KeypointStatus::VALID &&
        right_keypoints_rectified[i].first == KeypointStatus::VALID) {
      KeypointCV left_px = left_keypoints_rectified[i].second;
      KeypointCV right_px = right_keypoints_rectified[i].second;
      double disparity = left_px.x - right_px.x;
      if (disparity >= 0.0) {
        // Valid.
        nrValidDepths += 1;
        double depth = fx_b / disparity;
        if (depth < stereo_matching_params.min_point_dist_ ||
            depth > stereo_matching_params.max_point_dist_) {
          right_keypoints_rectified[i].first = KeypointStatus::NO_DEPTH;
          depths.push_back(0.0);
        } else {
          depths.push_back(depth);
        }
      } else {
        // Right match was wrong.
        right_keypoints_rectified[i].first = KeypointStatus::NO_DEPTH;
        depths.push_back(0.0);
      }
    } else {
      // Something is wrong.
      if (left_keypoints_rectified[i].first != KeypointStatus::VALID) {
        // We cannot have a valid right, without a valid left keypoint.
        right_keypoints_rectified[i].first = left_keypoints_rectified[i].first;
      }
      depths.push_back(0.0);
    }
  }
  CHECK_EQ(left_keypoints_rectified.size(), depths.size())
      << "getDepthFromRectifiedMatches: depths size mismatch!";

  return depths;
}

void StereoFrame::getSmartStereoMeasurements(
    StereoMeasurements* smart_stereo_measurements,
    const bool& use_stereo_measurements) const {
  CHECK_NOTNULL(smart_stereo_measurements)->clear();

  // Sanity check first.
  CHECK(is_rectified_) << "Stereo pair is not rectified";
  // Checks dimensionality of the feature vectors. This may be expensive!
  checkStereoFrame();

  const LandmarkIds& landmark_ids = left_frame_.landmarks_;
  smart_stereo_measurements->reserve(landmark_ids.size());
  for (size_t i = 0u; i < landmark_ids.size(); i++) {
    const LandmarkId& lmk_id = landmark_ids.at(i);
    if (lmk_id == -1) {
      continue;  // skip invalid points
    }

    // TODO implicit conversion float to double increases floating-point
    // precision!
    KeypointCV& left_kpt = left_keypoints_rectified_.at(i).second;
    const double& uL = left_kpt.x;
    const double& v = left_kpt.y;
    // Initialize to missing pixel information.
    double uR = std::numeric_limits<double>::quiet_NaN();
    LOG_IF_EVERY_N(WARNING, !use_stereo_measurements, 10)
        << "Dropping stereo information: uR = NaN! (set "
           "useStereoTracking_ = true to use it)";

    if (use_stereo_measurements &&
        right_keypoints_rectified_.at(i).first == KeypointStatus::VALID) {
      // TODO implicit conversion float to double increases floating-point
      // precision!
      uR = right_keypoints_rectified_.at(i).x;
    }
    smart_stereo_measurements->push_back(
        std::make_pair(lmk_id, gtsam::StereoPoint2(uL, uR, v)));
  }
}

void StereoFrame::print() const {
  LOG(INFO) << "=====================\n"
            << "id_: " << id_ << '\n'
            << "timestamp_: " << timestamp_ << '\n'
            << "isKeyframe_: " << is_keyframe_ << '\n'
            << "nr keypoints in left: " << left_frame_.keypoints_.size() << '\n'
            << "nr keypoints in right: " << right_frame_.keypoints_.size()
            << '\n'
            << "nr keypoints_3d_: " << keypoints_3d_.size() << '\n'
            << "left_frame_.cam_param_.body_Pose_cam_: "
            << left_frame_.cam_param_.body_Pose_cam_ << '\n'
            << "right_frame_.cam_param_.body_Pose_cam_: "
            << right_frame_.cam_param_.body_Pose_cam_;
}

cv::Mat StereoFrame::drawLeftRightCornersMatches(
    const std::vector<cv::DMatch>& matches,
    const bool& random_color) const {
  return UtilsOpenCV::DrawCornersMatches(left_img_rectified_,
                                         left_keypoints_rectified_,
                                         right_img_rectified_,
                                         right_keypoints_rectified_,
                                         matches,
                                         random_color);
}

void StereoFrame::showOriginal(const int verbosity) const {
  showImagesSideBySide(
      left_frame_.img_, right_frame_.img_, "original: left-right", verbosity);
}

void StereoFrame::showRectified(const bool& visualize,
                                const bool& write) const {
  CHECK(is_rectified_);
  cv::Mat canvas_undistorted_rectified =
      drawEpipolarLines(left_img_rectified_, right_img_rectified_, 15);
  if (write) {
    std::string img_name =
        "./outputImages/rectified_" + std::to_string(id_) + ".png";
    cv::imwrite(img_name, canvas_undistorted_rectified);
  }
  if (visualize) {
    cv::imshow("Rectified!", canvas_undistorted_rectified);
    cv::waitKey(1);
  }
}

cv::Mat StereoFrame::drawEpipolarLines(const cv::Mat img1,
                                       const cv::Mat img2,
                                       const int& num_lines,
                                       const bool& write) const {
  cv::Mat canvas = UtilsOpenCV::concatenateTwoImages(img1, img2);
  int line_gap = canvas.rows / (num_lines + 1);
  for (int l = 0; l < num_lines; l++) {
    float y_pos = (l + 1) * line_gap;
    cv::line(canvas,
             cv::Point2f(0.0f, y_pos),
             cv::Point2f(canvas.cols - 1, y_pos),
             cv::Scalar(0, 255, 0));
  }

  if (write) {
    std::string img_name =
        "./outputImages/drawEpipolarLines_" + std::to_string(id_) + ".png";
    cv::imwrite(img_name, canvas);
  }

  return canvas;
}

void StereoFrame::showLeftRightMatches() const {
  CHECK_EQ(left_frame_.keypoints_.size(), right_frame_.keypoints_.size())
      << "displayLeftRightMatches: error -  nr of corners in left and right "
         "cameras must be the same";

  // Draw the matchings: assumes that keypoints in the left and right keyframe
  // are ordered in the same way
  std::vector<cv::DMatch> matches;
  for (size_t i = 0; i < left_frame_.keypoints_.size(); i++) {
    matches.push_back(cv::DMatch(i, i, 0));
  }
  cv::Mat match_vis = UtilsOpenCV::DrawCornersMatches(left_frame_.img_,
                                                      left_frame_.keypoints_,
                                                      right_frame_.img_,
                                                      right_frame_.keypoints_,
                                                      matches);
  cv::imshow("match_visualization", match_vis);
  cv::waitKey(1);
}

void StereoFrame::printKeypointStats(
    const StatusKeypointsCV& right_keypoints_rectified) const {
  size_t n_valid = 0u;
  size_t n_no_left_rect = 0u;
  size_t n_no_right_rect = 0u;
  size_t n_no_depth = 0u;
  size_t n_failed_arun_rkp = 0u;
  for (const StatusKeypointCV& right_keypoint_rectified :
       right_keypoints_rectified) {
    switch (right_keypoint_rectified.first) {
      case KeypointStatus::VALID: {
        n_valid++;
        break;
      }
      case KeypointStatus::NO_LEFT_RECT: {
        n_no_left_rect++;
        break;
      }
      case KeypointStatus::NO_RIGHT_RECT: {
        n_no_right_rect++;
        break;
      }
      case KeypointStatus::NO_DEPTH: {
        n_no_depth++;
        break;
      }
      case KeypointStatus::FAILED_ARUN: {
        n_failed_arun_rkp++;
        break;
      }
    }
  }
  LOG(INFO) << "Nr of right keypoints: " << right_keypoints_rectified.size()
            << " of which:\n"
            << "nrValid: " << n_valid << '\n'
            << "nrNoLeftRect: " << n_no_left_rect << '\n'
            << "nrNoRightRect: " << n_no_right_rect << '\n'
            << "nrNoDepth: " << n_no_depth << '\n'
            << "nrFailedArunRKP: " << n_failed_arun_rkp;
}

}  // namespace VIO
