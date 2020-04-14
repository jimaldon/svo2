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

#include <algorithm>
#include <vikit/math_utils.h>
#include <vikit/abstract_camera.h>
#include <vikit/vision.h>
#include <svo/global.h>
#include <svo/depth_filter.h>
#include <svo/frame.h>
#include <svo/point.h>
#include <svo/feature.h>
#include <svo/matcher.h>
#include <svo/config.h>
#define _USE_MATH_DEFINES
#include <cmath>

using namespace std;

namespace svo {

int Seed::batch_counter = 0;
int Seed::seed_counter = 0;

Seed::Seed(std::unique_ptr<Feature> ftr, float depth_mean, float depth_min) :
    batch_id(batch_counter),
    id(seed_counter++),
    ftr(std::move(ftr)),
    a(10),
    b(10),
    mu(1.0/depth_mean),
    z_range(1.0/depth_min),
    sigma2(z_range*z_range/36)
{}

DepthFilter::DepthFilter(std::shared_ptr<vilib::DetectorBaseGPU> feature_detector, callback_t seed_converged_cb) :
    feature_detector_(feature_detector),
    seed_converged_cb_(seed_converged_cb),
    seeds_updating_halt_(false),
    new_keyframe_set_(false),
    new_keyframe_min_depth_(0.0),
    new_keyframe_mean_depth_(0.0)
{}

DepthFilter::~DepthFilter()
{
    stopThread();
    SVO_INFO_STREAM("DepthFilter destructed.");
}

void DepthFilter::startThread()
{
    thread_halt_ = false;
    thread_ = make_unique<thread>(&DepthFilter::updateSeedsLoop, this);
}

void DepthFilter::stopThread()
{
    SVO_INFO_STREAM("DepthFilter stop thread invoked.");
    if(thread_)
    {
        SVO_INFO_STREAM("DepthFilter interrupt and join thread... ");
        seeds_updating_halt_ = true;
        thread_halt_ = true;
        thread_->join();
        thread_.reset();
    }
}

void DepthFilter::addFrame(FramePtr frame)
{
  if(thread_)
  {
    {
      lock_t lock(frame_queue_mut_);
      if(frame_queue_.size() > 2)
        frame_queue_.pop();
      frame_queue_.push(frame);
    }
    seeds_updating_halt_ = false;
    frame_queue_cond_.notify_one();
  }
  else
    updateSeeds(frame);
}

void DepthFilter::addKeyframe(FramePtr frame, double depth_mean, double depth_min)
{
    new_keyframe_min_depth_ = depth_min;
    new_keyframe_mean_depth_ = depth_mean;
    if(thread_)
    {
        new_keyframe_ = frame;
        new_keyframe_set_ = true;
        seeds_updating_halt_ = true;
        frame_queue_cond_.notify_one();
    }
    else
        initializeSeeds(frame);
}

void DepthFilter::addKeyframe(FramePtr frame, double depth_mean, double depth_min, const std::vector<FramePtr> & history_frames)
{
    new_keyframe_min_depth_ = depth_min;
    new_keyframe_mean_depth_ = depth_mean;
    if(thread_)
    {
        new_keyframe_ = frame;
        new_keyframe_set_ = true;
        seeds_updating_halt_ = true;
        new_keyframe_update_frames_ = history_frames;
        frame_queue_cond_.notify_one();
    }
    else
        initializeSeeds(frame);
}

void DepthFilter::initializeSeeds(FramePtr frame)
{
    using namespace std;
    auto &grid = feature_detector_->getGrid();
    for_each(frame->fts_.begin(), frame->fts_.end(), [&](const std::unique_ptr<Feature> &f){
        grid.setOccupied(static_cast<int>(f->px[0]), static_cast<int>(f->px[1]));
    });
    feature_detector_->detect(frame->pyramid_);
    const auto &pts = feature_detector_->getPoints();

    // initialize a seed for every new feature
    seeds_updating_halt_ = true;
    lock_t lock(seeds_mut_); // by locking the updateSeeds function stops
    ++Seed::batch_counter;
    for_each(pts.begin(), pts.end(), [&](const vilib::DetectorBase::FeaturePoint &pt){
        seeds_.emplace_back(make_unique<Feature>(frame.get(), Vector2d(pt.x_, pt.y_), pt.level_), new_keyframe_mean_depth_, new_keyframe_min_depth_);
    });

    if(options_.verbose)
        SVO_INFO_STREAM("DepthFilter: Initialized " << pts.size() << " new seeds");
    seeds_updating_halt_ = false;
}

void DepthFilter::removeKeyframe(FramePtr frame)
{
  seeds_updating_halt_ = true;
  lock_t lock(seeds_mut_);
  std::list<Seed>::iterator it=seeds_.begin();
  size_t n_removed = 0;
  while(it!=seeds_.end())
  {
    if(it->ftr->frame == frame.get())
    {
      it = seeds_.erase(it);
      ++n_removed;
    }
    else
      ++it;
  }
  seeds_updating_halt_ = false;
}

void DepthFilter::reset()
{
    seeds_updating_halt_ = true;
    {
        lock_t lock(seeds_mut_);
        seeds_.clear();
    }
    lock_t lock(seeds_mut_);
    while(!frame_queue_.empty())
        frame_queue_.pop();
    seeds_updating_halt_ = false;

    if(options_.verbose)
        SVO_INFO_STREAM("DepthFilter: RESET.");
}

void DepthFilter::updateSeedsLoop()
{
    while(!thread_halt_)
    {
        FramePtr frame;
        std::vector<FramePtr> history_frames;
        {
            lock_t lock(frame_queue_mut_);
            while(frame_queue_.empty() && new_keyframe_set_ == false)
                frame_queue_cond_.wait(lock);
            if(new_keyframe_set_)
            {
                new_keyframe_set_ = false;
                seeds_updating_halt_ = false;
                clearFrameQueue();
                frame = new_keyframe_;
                history_frames = new_keyframe_update_frames_;
            }
            else
            {
                frame = frame_queue_.front();
                frame_queue_.pop();
            }
        }
        updateSeeds(frame);
        if(frame->isKeyframe())
        {
            int old_seed_size = seeds_.size();
            initializeSeeds(frame);
            int new_seed_size = seeds_.size();
            if(new_seed_size>old_seed_size)
            {
                for(auto& f:new_keyframe_update_frames_)
                    updateSeeds(f, old_seed_size);
            }
        }
    }
}

void DepthFilter::updateSeeds(FramePtr frame, int start_seed_idx)
{
  // update only a limited number of seeds, because we don't have time to do it
  // for all the seeds in every frame!
  size_t n_updates=0, n_failed_matches=0, n_seeds = seeds_.size();
  lock_t lock(seeds_mut_);
  std::list<Seed>::iterator it=seeds_.begin();
  for(int i=0; i<start_seed_idx; i++) {it++;}

  const double focal_length = frame->cam_->errorMultiplier2();
  double px_noise = 1.0;
  double px_error_angle = atan(px_noise/(2.0*focal_length))*2.0; // law of chord (sehnensatz)

  while( it!=seeds_.end())
  {
    // set this value true when seeds updating should be interrupted
    if(seeds_updating_halt_)
      return;

    // check if seed is not already too old
    if((Seed::batch_counter - it->batch_id) > options_.max_n_kfs) {
      it = seeds_.erase(it);
      continue;
    }

    // check if point is visible in the current image
    SE3 T_ref_cur = it->ftr->frame->T_f_w_ * frame->T_f_w_.inverse();
    const Vector3d xyz_f(T_ref_cur.inverse()*(1.0/it->mu * it->ftr->f) );
    if(xyz_f.z() < 0.0)  {
      ++it; // behind the camera
      continue;
    }
    if(!frame->cam_->isInFrame(frame->f2c(xyz_f).cast<int>())) {
      ++it; // point does not project in image
      continue;
    }

    // we are using inverse depth coordinates
    float z_inv_min = it->mu + sqrt(it->sigma2);
    float z_inv_max = std::max(it->mu - sqrt(it->sigma2), 0.00000001f);
    double z;
    if(!matcher_.findEpipolarMatchDirect(
        *it->ftr->frame, *frame, *it->ftr, 1.0/it->mu, 1.0/z_inv_min, 1.0/z_inv_max, z))
    {
      it->b++; // increase outlier probability when no match was found
      ++it;
      ++n_failed_matches;
      continue;
    }

    // compute tau
    double tau = computeTau(T_ref_cur, it->ftr->f, z, px_error_angle);
    double tau_inverse = 0.5 * (1.0/std::max(0.0000001, z-tau) - 1.0/(z+tau));

    // update the estimate
    updateSeed(1./z, tau_inverse*tau_inverse, &*it);
    ++n_updates;

    if(frame->isKeyframe())
    {
        // The feature detector should not initialize new seeds close to this location
        auto &px = matcher_.px_cur_;
        feature_detector_->getGrid().setOccupied(px[0], px[1]);
    }

    // if the seed has converged, we initialize a new candidate point and remove the seed
    if(sqrt(it->sigma2) < it->z_range/options_.seed_convergence_sigma2_thresh)
    {
      assert(it->ftr->point == NULL); // TODO this should not happen anymore
      Vector3d xyz_world(it->ftr->frame->T_f_w_.inverse() * (it->ftr->f * (1.0/it->mu)));
      Point* point = new Point(xyz_world, it->ftr.get());
      it->ftr->point = point;
      /* FIXME it is not threadsafe to add a feature to the frame here.
      if(frame->isKeyframe())
      {
        Feature* ftr = new Feature(frame.get(), matcher_.px_cur_, matcher_.search_level_);
        ftr->point = point;
        point->addFrameRef(ftr);
        frame->addFeature(ftr);
        it->ftr->frame->addFeature(it->ftr);
      }
      else
      */
      {
        seed_converged_cb_(point, it->sigma2); // put in candidate list
      }
      it = seeds_.erase(it);
    }
    else if(isnan(z_inv_min))
    {
      SVO_WARN_STREAM("z_min is NaN");
      it = seeds_.erase(it);
    }
    else
      ++it;
  }
}

void DepthFilter::clearFrameQueue()
{
  while(!frame_queue_.empty())
    frame_queue_.pop();
}

/*void DepthFilter::getSeedsCopy(const FramePtr& frame, std::list<Seed>& seeds)
{
    lock_t lock(seeds_mut_);
    for(std::list<Seed>::iterator it=seeds_.begin(); it!=seeds_.end(); ++it)
    {
        if (it->ftr->frame == frame.get())
            seeds.push_back(*it);
    }
}*/

template<class real>
real pdf(std::normal_distribution<real> nd, real x)
{
    real e = x - nd.mean(), sd = nd.stddev();
    e *= -e;
    e /= 2 * sd * sd;
    return exp(e) / (sd * sqrt(2 * M_PI));
}

void DepthFilter::updateSeed(const float x, const float tau2, Seed* seed)
{
  float norm_scale = sqrt(seed->sigma2 + tau2);
  if(std::isnan(norm_scale))
    return;
  std::normal_distribution<float> nd(seed->mu, norm_scale);
  float s2 = 1./(1./seed->sigma2 + 1./tau2);
  float m = s2*(seed->mu/seed->sigma2 + x/tau2);
  float C1 = seed->a/(seed->a+seed->b) * pdf(nd, x);
  float C2 = seed->b/(seed->a+seed->b) * 1./seed->z_range;
  float normalization_constant = C1 + C2;
  C1 /= normalization_constant;
  C2 /= normalization_constant;
  float f = C1*(seed->a+1.)/(seed->a+seed->b+1.) + C2*seed->a/(seed->a+seed->b+1.);
  float e = C1*(seed->a+1.)*(seed->a+2.)/((seed->a+seed->b+1.)*(seed->a+seed->b+2.))
          + C2*seed->a*(seed->a+1.0f)/((seed->a+seed->b+1.0f)*(seed->a+seed->b+2.0f));

  // update parameters
  float mu_new = C1*m+C2*seed->mu;
  seed->sigma2 = C1*(s2 + m*m) + C2*(seed->sigma2 + seed->mu*seed->mu) - mu_new*mu_new;
  seed->mu = mu_new;
  seed->a = (e-f)/(f-e/f);
  seed->b = seed->a*(1.0f-f)/f;
}

double DepthFilter::computeTau(
      const SE3d& T_ref_cur,
      const Vector3d& f,
      const double z,
      const double px_error_angle)
{
  Vector3d t(T_ref_cur.translation());
  Vector3d a = f*z-t;
  double t_norm = t.norm();
  double a_norm = a.norm();
  double alpha = acos(f.dot(t)/t_norm); // dot product
  double beta = acos(a.dot(-t)/(t_norm*a_norm)); // dot product
  double beta_plus = beta + px_error_angle;
  double gamma_plus = PI-alpha-beta_plus; // triangle angles sum to PI
  double z_plus = t_norm*sin(beta_plus)/sin(gamma_plus); // law of sines
  return (z_plus - z); // tau
}

} // namespace svo
