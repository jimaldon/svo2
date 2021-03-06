// This file is part of SVO - Semi-direct Visual Odometry.
//
// Copyright (C) 2014 Christian Forster <forster at ifi dot uzh dot ch>
// (Robotics and Perception Group, University of Zurich, Switzerland).
//
// SVO is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or any later version.
//
// SVO is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <visualize.h>
#include <handlers.h>
#include <svo/frame.h>
#include <svo/point.h>
#include <svo/map.h>
#include <svo/feature.h>
#include <iostream>
#include <fstream>
#include <vikit/timer.h>
//#include <vikit/output_helper.h>
//#include <vikit/params_helper.h>
#include <deque>
#include <algorithm>
#include <pybind11/numpy.h>

namespace svo {

/*Visualizer::
Visualizer() :
    pnh_("~"),
    trace_id_(0),
    img_pub_level_(vk::getParam<int>("svo/publish_img_pyr_level", 0)),
    img_pub_nth_(vk::getParam<int>("svo/publish_every_nth_img", 1)),
    dense_pub_nth_(vk::getParam<int>("svo/publish_every_nth_dense_input", 1)),
    publish_world_in_cam_frame_(vk::getParam<bool>("svo/publish_world_in_cam_frame", true)),
    publish_map_every_frame_(vk::getParam<bool>("svo/publish_map_every_frame", false)),
    publish_points_display_time_(vk::getParam<double>("svo/publish_point_display_time", 0)),
    T_world_from_vision_(Matrix3d::Identity(), Vector3d::Zero())
{
  // Init ROS Marker Publishers
  pub_frames_ = pnh_.advertise<visualization_msgs::Marker>("keyframes", 10);
  pub_points_ = pnh_.advertise<visualization_msgs::Marker>("points", 1000);
  pub_pose_ = pnh_.advertise<geometry_msgs::PoseWithCovarianceStamped>("pose",10);
  pub_info_ = pnh_.advertise<svo_msgs::Info>("info", 10);
  pub_dense_ = pnh_.advertise<svo_msgs::DenseInput>("dense_input",10);

  // create video publisher
  image_transport::ImageTransport it(pnh_);
  pub_images_ = it.advertise("image", 10);
}*/

using namespace std;

cv::Mat visualizeMinimal(const cv::Mat& img,
    const FramePtr& frame,
    const FrameHandlerMono& slam,
    size_t level)
{
    if(!frame)
        return img;

    const int scale = (1 << level);
    cv::Mat img_rgb(frame->pyramid_[level].size(), CV_8UC3);
    cv::Mat l_img = frame->pyramid_[level];
    cv::cvtColor(l_img, img_rgb, cv::COLOR_GRAY2RGB);

    if(slam.stage() == FrameHandlerBase::STAGE_SECOND_FRAME)
    {
        // During initialization, draw lines.
        const vector<cv::Point2f>& px_ref(slam.initFeatureTrackRefPx());
        const vector<cv::Point2f>& px_cur(slam.initFeatureTrackCurPx());
        for(vector<cv::Point2f>::const_iterator it_ref = px_ref.begin(), it_cur = px_cur.begin(); it_ref != px_ref.end(); ++it_ref, ++it_cur)
            cv::line(
                        img_rgb,
                        cv::Point2f(it_cur->x/scale, it_cur->y/scale),
                        cv::Point2f(it_ref->x/scale, it_ref->y/scale),
                        cv::Scalar(0,255,0), 2
                        );
    }

    if(level == 0)
    {
        for(Features::iterator it=frame->fts_.begin(); it!=frame->fts_.end(); ++it)
        {
            if((*it)->type == Feature::EDGELET)
                cv::line(img_rgb,
                         cv::Point2f((*it)->px[0]+3*(*it)->grad[1], (*it)->px[1]-3*(*it)->grad[0]),
                        cv::Point2f((*it)->px[0]-3*(*it)->grad[1], (*it)->px[1]+3*(*it)->grad[0]),
                        cv::Scalar(255,0,255), 2);
            else//point size 5x5
                cv::rectangle(img_rgb,
                              cv::Point2f((*it)->px[0]-2, (*it)->px[1]-2),
                        cv::Point2f((*it)->px[0]+2, (*it)->px[1]+2),
                        cv::Scalar(0,255,0), cv::FILLED);
        }
    }
    else if(level == 1){//point size 3x3
        for(Features::iterator it = frame->fts_.begin(); it != frame->fts_.end(); ++it)
            cv::rectangle(img_rgb,
                          cv::Point2f((*it)->px[0] / scale - 1, (*it)->px[1] / scale - 1),
                          cv::Point2f((*it)->px[0] / scale + 1, (*it)->px[1] / scale + 1),
                          cv::Scalar(0, 255, 0), cv::FILLED);
    }else{ //point size 1x1
        for(Features::iterator it = frame->fts_.begin(); it != frame->fts_.end(); ++it){
            cv::Vec3b &p = img_rgb.at<cv::Vec3b>((*it)->px[1] / scale, (*it)->px[0] / scale);
            p[0] = 0; p[1] = 255; p[2] = 0;
        }
    }
    cv::imshow("img_rgb", img_rgb);
    return img_rgb;
}


/*
void Visualizer::visualizeMarkers(
    const FramePtr& frame,
    const set<FramePtr>& core_kfs,
    const Map& map)
{
  if(frame == NULL)
    return;

  vk::output_helper::publishTfTransform(
      frame->T_f_w_*T_world_from_vision_.inverse(),
      ros::Time(frame->timestamp_), "cam_pos", "world", br_);

  if(pub_frames_.getNumSubscribers() > 0 || pub_points_.getNumSubscribers() > 0)
  {
    vk::output_helper::publishCameraMarker(
        pub_frames_, "cam_pos", "cams", ros::Time(frame->timestamp_),
        1, 0.6, Vector3d(0.,0.,1.));
    vk::output_helper::publishPointMarker(
        pub_points_, T_world_from_vision_*frame->pos(), "trajectory",
        ros::Time::now(), trace_id_, 0, 0.06, Vector3d(0.,0.,0.5));
    if(frame->isKeyframe() || publish_map_every_frame_)
      publishMapRegion(core_kfs);
    removeDeletedPts(map);
  }
}

void Visualizer::publishMapRegion(set<FramePtr> frames)
{
  if(pub_points_.getNumSubscribers() > 0)
  {
    int ts = vk::Timer::getCurrentTime();
    for(set<FramePtr>::iterator it=frames.begin(); it!=frames.end(); ++it)
      displayKeyframeWithMps(*it, ts);
  }
}

void Visualizer::removeDeletedPts(const Map& map)
{
  if(pub_points_.getNumSubscribers() > 0)
  {
    for(list<Point*>::const_iterator it=map.trash_points_.begin(); it!=map.trash_points_.end(); ++it)
      vk::output_helper::publishPointMarker(pub_points_, Vector3d(), "pts", ros::Time::now(), (*it)->id_, 2, 0.06, Vector3d());
  }
}

void Visualizer::displayKeyframeWithMps(const FramePtr& frame, int ts)
{
  // publish keyframe
  SE3 T_world_cam(T_world_from_vision_*frame->T_f_w_.inverse());
  vk::output_helper::publishFrameMarker(
      pub_frames_, T_world_cam.rotation_matrix(),
      T_world_cam.translation(), "kfs", ros::Time::now(), frame->id_*10, 0, 0.25);

  // publish point cloud and links
  for(Features::iterator it=frame->fts_.begin(); it!=frame->fts_.end(); ++it)
  {
    if((*it)->point == NULL)
      continue;

    if((*it)->point->last_published_ts_ == ts)
      continue;

    vk::output_helper::publishPointMarker(
        pub_points_, T_world_from_vision_*(*it)->point->pos_, "pts",
        ros::Time::now(), (*it)->point->id_, 0, 0.2, Vector3d(1.0, 0., 1.0),
        publish_points_display_time_);
    (*it)->point->last_published_ts_ = ts;
  }
}

void Visualizer::exportToDense(const FramePtr& frame)
{
  // publish air_ground_msgs
  if(frame != NULL && dense_pub_nth_ > 0
      && trace_id_%dense_pub_nth_ == 0 && pub_dense_.getNumSubscribers() > 0)
  {
    svo_msgs::DenseInput msg;
    msg.header.stamp = ros::Time(frame->timestamp_);
    msg.header.frame_id = "/world";
    msg.frame_id = frame->id_;

    cv_bridge::CvImage img_msg;
    img_msg.header.stamp = msg.header.stamp;
    img_msg.header.frame_id = "camera";
    img_msg.image = frame->img();
    img_msg.encoding = sensor_msgs::image_encodings::MONO8;
    msg.image = *img_msg.toImageMsg();

    double min_z = std::numeric_limits<double>::max();
    double max_z = std::numeric_limits<double>::min();
    for(Features::iterator it=frame->fts_.begin(); it!=frame->fts_.end(); ++it)
    {
      if((*it)->point==NULL)
        continue;
      Vector3d pos = frame->T_f_w_*(*it)->point->pos_;
      min_z = fmin(pos[2], min_z);
      max_z = fmax(pos[2], max_z);
    }
    msg.min_depth = (float) min_z;
    msg.max_depth = (float) max_z;

    // publish cam in world frame
    SE3 T_world_from_cam(T_world_from_vision_*frame->T_f_w_.inverse());
    Quaterniond q(T_world_from_cam.rotation_matrix());
    Vector3d p(T_world_from_cam.translation());

    msg.pose.position.x = p[0];
    msg.pose.position.y = p[1];
    msg.pose.position.z = p[2];
    msg.pose.orientation.w = q.w();
    msg.pose.orientation.x = q.x();
    msg.pose.orientation.y = q.y();
    msg.pose.orientation.z = q.z();
    pub_dense_.publish(msg);
  }
}*/

} // end namespace svo

py::array_t<uchar> visualizeMinimalWrapper(
        //const cv::Mat& img,
        py::array_t<uchar> img,
        //const svo::FramePtr& frame,
        std::shared_ptr<FHMWrapper> slam,
        size_t level)
{
    if (img.ndim() != 3)
        assert(false);
    int h = img.shape(0), w = img.shape(1), c = img.shape(2);
    size_t strides[] = {(size_t)img.strides(0), (size_t)img.strides(1), (size_t)img.strides(2)};
    std::vector<int> sizes = {h, w, c};
    cv::Mat img_mat(sizes, CV_8U, (void *)img.data(), strides);
    svo::visualizeMinimal(img_mat, slam->lastFrame(), *slam, level);
    return img;
}

void initVisualize(py::module &m)
{
    auto m_visualize = m.def_submodule("visualize");
    m_visualize.def("visualizeMinimal", &visualizeMinimalWrapper);
}
