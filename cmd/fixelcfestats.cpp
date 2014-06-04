/*
    Copyright 2012 Brain Research Institute, Melbourne, Australia

    Written by David Raffelt, 01/10/2012.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "command.h"
#include "progressbar.h"
#include "thread/exec.h"
#include "thread/queue.h"
#include "image/loop.h"
#include "image/voxel.h"
#include "image/buffer.h"
#include "image/buffer_preload.h"
#include "image/transform.h"
#include "image/sparse/fixel_metric.h"
#include "image/sparse/voxel.h"
#include "math/vector.h"
#include "math/matrix.h"
#include "math/stats/permutation.h"
#include "math/stats/glm.h"
#include "timer.h"
#include "stats/tfce.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/scalar_file.h"
#include "dwi/tractography/mapping/mapper.h"
#include "dwi/tractography/mapping/loader.h"
#include "dwi/tractography/mapping/writer.h"


using namespace MR;
using namespace App;
using namespace MR::DWI::Tractography::Mapping;
using Image::Sparse::FixelMetric;

typedef float value_type;

void usage ()
{
  AUTHOR = "David Raffelt (d.raffelt@brain.org.au)";

  DESCRIPTION
  + "Statistical analysis of fixel-specific measures using fixel-based connectivity enhancement and non-parametric permutation testing.";


  ARGUMENTS
  + Argument ("input", "a text file listing the file names of the input fixel images").type_file()

  + Argument ("template", "the fixel mask used to define fixels of interest. This can be generated by "
                          "thresholding the group average AFD fixel image.").type_image_in()

  + Argument ("design", "the design matrix").type_file()

  + Argument ("contrast", "the contrast matrix").type_file()

  + Argument ("tracks", "the tracks used to determine fixel-fixel connectivity").type_file()

  + Argument ("output", "the filename prefix for all output.").type_text();


  OPTIONS

  + Option ("notest", "don't perform permutation testing and only output population statistics (effect size, stdev etc)")

  + Option ("nperms", "the number of permutations (default = 5000).")
  + Argument ("num").type_integer (1, 5000, 100000)

  + Option ("dh", "the height increment used in the TFCE integration (default = 0.1)")
  + Argument ("value").type_float (0.001, 0.1, 100000)

  + Option ("tfce_e", "TFCE extent parameter (default = 2.0)")
  + Argument ("value").type_float (0.0, 2.0, 100000)

  + Option ("tfce_h", "TFCE height parameter (default = 1.0)")
  + Argument ("value").type_float (0.0, 1.0, 100000)

  + Option ("tfce_c", "TFCE connectivity parameter (default = 0.5)")
  + Argument ("value").type_float (0.0, 0.5, 100000)

  + Option ("angle", "the max angle threshold for computing inter-subject fixel correspondence")
  + Argument ("value").type_float (0.0, 30, 90)

  + Option ("connectivity", "a threshold to define the required fraction of shared connections to be included in the neighbourhood (default: 1%)")
  + Argument ("threshold").type_float (0.001, 0.01, 1.0)

  + Option ("smooth", "smooth the fixel value along the fibre tracts using a Gaussian kernel with the supplied FWHM (default: 10mm)")
  + Argument ("FWHM").type_float (0.0, 10.0, 200.0);

}


/**
 * Process each track by converting each streamline to a set of dixels, and map these to fixels.
 */
class TrackProcessor {

  public:
    TrackProcessor (Image::BufferScratch<int32_t>& fixel_indexer,
                    const std::vector<Point<value_type> >& fixel_directions,
                    std::vector<uint16_t>& fixel_TDI,
                    std::vector<std::map<int32_t, Stats::TFCE::connectivity> >& connectivity_matrix,
                    value_type angular_threshold):
                    fixel_indexer (fixel_indexer) ,
                    fixel_directions (fixel_directions),
                    fixel_TDI (fixel_TDI),
                    connectivity_matrix (connectivity_matrix) {
      angular_threshold_dp = cos (angular_threshold * (M_PI/180.0));
    }

    bool operator () (SetVoxelDir& in)
    {
      // For each voxel tract tangent, assign to a fixel
      std::vector<int32_t> tract_fixel_indices;
      for (SetVoxelDir::const_iterator i = in.begin(); i != in.end(); ++i) {
        Image::Nav::set_pos (fixel_indexer, *i);
        fixel_indexer[3] = 0;
        int32_t first_index = fixel_indexer.value();
        if (first_index >= 0) {
          fixel_indexer[3] = 1;
          int32_t last_index = first_index + fixel_indexer.value();
          int32_t closest_fixel_index = -1;
          value_type largest_dp = 0.0;
          Point<value_type> dir (i->get_dir());
          dir.normalise();
          for (int32_t j = first_index; j < last_index; ++j) {
            value_type dp = Math::abs (dir.dot (fixel_directions[j]));
            if (dp > largest_dp) {
              largest_dp = dp;
              closest_fixel_index = j;
            }
          }
          if (largest_dp > angular_threshold_dp) {
            tract_fixel_indices.push_back (closest_fixel_index);
            fixel_TDI[closest_fixel_index]++;
          }
        }
      }

      for (size_t i = 0; i < tract_fixel_indices.size(); i++) {
        for (size_t j = i + 1; j < tract_fixel_indices.size(); j++) {
          connectivity_matrix[tract_fixel_indices[i]][tract_fixel_indices[j]].value++;
          connectivity_matrix[tract_fixel_indices[j]][tract_fixel_indices[i]].value++;
        }
      }

      return true;
    }

  private:
    Image::BufferScratch<int32_t>::voxel_type fixel_indexer;
    const std::vector<Point<value_type> >& fixel_directions;
    std::vector<uint16_t>& fixel_TDI;
    std::vector<std::map<int32_t, Stats::TFCE::connectivity> >& connectivity_matrix;
    value_type angular_threshold_dp;
};


void write_fixel_output (const std::string& filename,
                         const float* data,
                         const Image::Header& header,
                         Image::BufferSparse<FixelMetric>::voxel_type& mask_vox,
                         Image::BufferScratch<int32_t>::voxel_type& indexer_vox) {
  Image::BufferSparse<FixelMetric> output (filename, header);
  Image::BufferSparse<FixelMetric>::voxel_type output_voxel (output);
  Image::LoopInOrder loop (mask_vox);
  for (loop.start (mask_vox, indexer_vox, output_voxel); loop.ok(); loop.next (mask_vox, indexer_vox, output_voxel)) {
    output_voxel.value().zero();
    output_voxel.value().set_size (mask_vox.value().size());
    indexer_vox[3] = 0;
    int32_t index = indexer_vox.value();
    for (size_t f = 0; f != mask_vox.value().size(); ++f, ++index) {
     output_voxel.value()[f] = mask_vox.value()[f];
     output_voxel.value()[f].value = data[index];
    }
  }
}




void run() {

  Options opt = get_options ("dh");
  value_type dh = 0.1;
  if (opt.size())
    dh = opt[0][0];

  opt = get_options ("tfce_h");
  value_type tfce_H = 2.0;
  if (opt.size())
    tfce_H = opt[0][0];

  opt = get_options ("tfce_e");
  value_type tfce_E = 1.0;
  if (opt.size())
    tfce_E = opt[0][0];

  opt = get_options ("tfce_c");
  value_type tfce_C = 0.5;
  if (opt.size())
    tfce_C = opt[0][0];

  opt = get_options ("nperms");
  int num_perms = 5000;
  if (opt.size())
    num_perms = opt[0][0];

  value_type angular_threshold = 30.0;
  opt = get_options ("angle");
  if (opt.size())
    angular_threshold = opt[0][0];
  const float angular_threshold_dp = cos (angular_threshold * (M_PI/180.0));

  value_type connectivity_threshold = 0.01;
  opt = get_options ("connectivity");
  if (opt.size())
    connectivity_threshold = opt[0][0];

  value_type smooth_std_dev = 10.0 / 2.3548;
  opt = get_options ("smooth");
  if (opt.size())
    smooth_std_dev = value_type(opt[0][0]) / 2.3548;

  // Read filenames
  std::vector<std::string> filenames;
  {
    std::string folder = Path::dirname (argument[0]);
    std::ifstream ifs (argument[0].c_str());
    std::string temp;
    while (getline (ifs, temp))
      filenames.push_back (Path::join (folder, temp));
  }

  // Load design matrix:
  Math::Matrix<value_type> design;
  design.load (argument[2]);
  if (design.rows() != filenames.size())
    throw Exception ("number of subjects does not match number of rows in design matrix");

  // Load contrast matrix:
  Math::Matrix<value_type> contrast;
  contrast.load (argument[3]);

  if (contrast.columns() > design.columns())
    throw Exception ("too many contrasts for design matrix");
  contrast.resize (contrast.rows(), design.columns());

  Image::Header input_header (argument[1]);
  Image::BufferSparse<FixelMetric> mask (input_header);
  Image::BufferSparse<FixelMetric>::voxel_type mask_vox (mask);

  // Create an image to store the fixel indices  //TODO, if we had a fixel buffer scratch this would be cleaner
  Image::Header index_header (input_header);
  index_header.set_ndim(4);
  index_header.dim(3) = 2;
  Image::BufferScratch<int32_t> fixel_indexer (index_header);
  Image::BufferScratch<int32_t>::voxel_type indexer_vox (fixel_indexer);
  Image::LoopInOrder loop4D (indexer_vox);
  for (loop4D.start (indexer_vox); loop4D.ok(); loop4D.next (indexer_vox))
    indexer_vox.value() = -1;

  std::vector<Point<value_type> > positions;
  std::vector<Point<value_type> > directions;  //TODO could use fixel mask instead

  Image::Transform image_transform (indexer_vox);
  Image::LoopInOrder loop (mask_vox);
  for (loop.start (mask_vox, indexer_vox); loop.ok(); loop.next (mask_vox, indexer_vox)) {
    indexer_vox[3] = 0;
    indexer_vox.value() = directions.size();
    int32_t fixel_count = 0;
    for (size_t f = 0; f != mask_vox.value().size(); ++f, ++fixel_count) {
      directions.push_back (mask_vox.value()[f].dir);
      Point<value_type> pos;
      image_transform.voxel2scanner (mask_vox, pos);
      positions.push_back (pos);
    }
    indexer_vox[3] = 1;
    indexer_vox.value() = fixel_count;
  }

  uint32_t num_fixels = directions.size();
  CONSOLE ("number of fixels: " + str(num_fixels));

  // Compute fixel-fixel connectivity
  std::vector<std::map<int32_t, Stats::TFCE::connectivity> > connectivity_matrix (num_fixels);
  std::vector<uint16_t> fixel_TDI (num_fixels, 0.0);
  std::string track_filename = argument[4];
  std::string output_prefix = argument[5];
  DWI::Tractography::Properties properties;
  DWI::Tractography::Reader<value_type> track_file (track_filename, properties);
  // Read in tracts, and compute whole-brain fixel-fixel connectivity
  const size_t num_tracks = properties["count"].empty() ? 0 : to<int> (properties["count"]);
  if (!num_tracks)
    throw Exception ("no tracks found in input file");
  if (num_tracks < 1000000)
    WARN ("more than 1 million tracks should be used to ensure robust fixel-fixel connectivity");
  {
    typedef DWI::Tractography::Mapping::SetVoxelDir SetVoxelDir;
    DWI::Tractography::Mapping::TrackLoader loader (track_file, num_tracks, "pre-computing fixel-fixel connectivity...");
    DWI::Tractography::Mapping::TrackMapperBase<SetVoxelDir> mapper (input_header);
    TrackProcessor tract_processor (fixel_indexer, directions, fixel_TDI, connectivity_matrix, angular_threshold);
    Thread::run_queue (
        loader,
        Thread::batch (DWI::Tractography::Streamline<float>()),
        mapper,
        Thread::batch (SetVoxelDir()),
        tract_processor);
  }
  track_file.close();


  // Normalise connectivity matrix and threshold, pre-compute fixel-fixel weights for smoothing.
  std::vector<std::map<int32_t, value_type> > smoothing_weights (num_fixels);
  bool do_smoothing = false;
  const value_type gaussian_const2 = 2.0 * smooth_std_dev * smooth_std_dev;
  value_type gaussian_const1 = 1.0;
  if (smooth_std_dev > 0.0) {
    do_smoothing = true;
    gaussian_const1 = 1.0 / (smooth_std_dev *  Math::sqrt (2.0 * M_PI));
  }
  {
    ProgressBar progress ("normalising and thresholding fixel-fixel connectivity matrix...", num_fixels);
    for (unsigned int fixel = 0; fixel < num_fixels; ++fixel) {
      std::map<int32_t, Stats::TFCE::connectivity>::iterator it = connectivity_matrix[fixel].begin();
      while (it != connectivity_matrix[fixel].end()) {
        value_type connectivity = it->second.value / value_type (fixel_TDI[fixel]);
        if (connectivity < connectivity_threshold)  {
          connectivity_matrix[fixel].erase (it++);
        } else {
          if (do_smoothing) {
            value_type distance = Math::sqrt (Math::pow2 (positions[fixel][0] - positions[it->first][0]) +
                                              Math::pow2 (positions[fixel][1] - positions[it->first][1]) +
                                              Math::pow2 (positions[fixel][2] - positions[it->first][2]));
            value_type smoothing_weight = connectivity * gaussian_const1 * Math::exp (-Math::pow2 (distance) / gaussian_const2);
            if (smoothing_weight > connectivity_threshold)
              smoothing_weights[fixel].insert (std::pair<int32_t, value_type> (it->first, smoothing_weight));
          }
          it->second.value = Math::pow (connectivity, tfce_C);
          ++it;
        }
      }
      // Make sure the fixel is fully connected to itself giving it a smoothing weight of 1
      Stats::TFCE::connectivity self_connectivity;
      self_connectivity.value = 1.0;
      connectivity_matrix[fixel].insert (std::pair<int32_t, Stats::TFCE::connectivity> (fixel, self_connectivity));
      smoothing_weights[fixel].insert (std::pair<int32_t, value_type> (fixel, gaussian_const1));
      progress++;
    }
  }

  // Normalise smoothing weights
  for (size_t fixel = 0; fixel < num_fixels; ++fixel) {
    value_type sum = 0.0;
    for (std::map<int32_t, value_type>::iterator it = smoothing_weights[fixel].begin(); it != smoothing_weights[fixel].end(); ++it)
      sum += it->second;
    value_type norm_factor = 1.0 / sum;
    for (std::map<int32_t, value_type>::iterator it = smoothing_weights[fixel].begin(); it != smoothing_weights[fixel].end(); ++it)
      it->second *= norm_factor;
  }

  // Load input data
  Math::Matrix<value_type> data (num_fixels, filenames.size());
  {
    ProgressBar progress ("loading input images...", filenames.size());
    for (size_t subject = 0; subject < filenames.size(); subject++) {
      LogLevelLatch log_level (0);
      Image::BufferSparse<FixelMetric> fixel (filenames[subject]);
      Image::BufferSparse<FixelMetric>::voxel_type fixel_vox (fixel);
      Image::check_dimensions (fixel, mask, 0, 3);
      std::vector<value_type> temp_fixel_data (directions.size(), 0.0);

      for (loop.start (fixel_vox, indexer_vox); loop.ok(); loop.next (fixel_vox, indexer_vox)) {
         indexer_vox[3] = 0;
         int32_t index = indexer_vox.value();
         indexer_vox[3] = 1;
         int32_t number_fixels = indexer_vox.value();

         // for each fixel in the mask, find the corresponding fixel in this subject voxel
         for (int32_t i = index; i < index + number_fixels; ++i) {
           value_type largest_dp = 0.0;
           int index_of_closest_fixel = -1;
           for (size_t f = 0; f != fixel_vox.value().size(); ++f) {
             value_type dp = Math::abs (directions[i].dot(fixel_vox.value()[f].dir));
             if (dp > largest_dp) {
               largest_dp = dp;
               index_of_closest_fixel = f;
             }
           }
           if (largest_dp > angular_threshold_dp)
             temp_fixel_data[i] = fixel_vox.value()[index_of_closest_fixel].value;
         }
       }

      // Smooth the data
      for (size_t fixel = 0; fixel < directions.size(); ++fixel) {
        value_type value = 0.0;
        std::map<int32_t, value_type>::const_iterator it = smoothing_weights[fixel].begin();
        for (; it != smoothing_weights[fixel].end(); ++it)
          value += temp_fixel_data[it->first] * it->second;
        data (fixel, subject) = value;
      }
      progress++;
    }
  }

  CONSOLE ("outputting beta coefficients, effect size and standard deviation");
  Math::Matrix<float> temp;

  Math::Stats::GLM::solve_betas (data, design, temp);
  for (size_t i = 0; i < contrast.columns(); ++i)
    write_fixel_output (output_prefix + "_beta" + str(i) + ".msf", temp.column (i).ptr(), input_header, mask_vox, indexer_vox);

  Math::Stats::GLM::abs_effect_size (data, design, contrast, temp);
  write_fixel_output (output_prefix + "_abs_effect.msf", temp.ptr(), input_header, mask_vox, indexer_vox);
  Math::Stats::GLM::std_effect_size (data, design, contrast, temp);
  write_fixel_output (output_prefix + "_std_effect.msf", temp.ptr(), input_header, mask_vox, indexer_vox);
  Math::Stats::GLM::stdev (data, design, temp);
  write_fixel_output (output_prefix + "_std_dev.msf", temp.ptr(), input_header, mask_vox, indexer_vox);



  // Perform permutation testing
  opt = get_options ("notest");
  if (!opt.size()) {
     int num_fixels = directions.size();
     Math::Vector<value_type> perm_distribution_pos (num_perms - 1);
     Math::Vector<value_type> perm_distribution_neg (num_perms - 1);
     std::vector<value_type> tfce_output_pos (num_fixels, 0.0);
     std::vector<value_type> tfce_output_neg (num_fixels, 0.0);
     std::vector<value_type> tvalue_output (num_fixels, 0.0);
     std::vector<value_type> pvalue_output_pos (num_fixels, 0.0);
     std::vector<value_type> pvalue_output_neg (num_fixels, 0.0);

     Math::Stats::GLMTTest glm_ttest (data, design, contrast);
     {
       Stats::TFCE::Connectivity tfce_integrator (connectivity_matrix, dh, tfce_E, tfce_H);
       Stats::TFCE::run (glm_ttest, tfce_integrator, num_perms,
                         perm_distribution_pos, perm_distribution_neg,
                         tfce_output_pos, tfce_output_neg, tvalue_output);
     }

     perm_distribution_pos.save (output_prefix + "_perm_dist_pos.txt");
     perm_distribution_neg.save (output_prefix + "_perm_dist_neg.txt");
     Math::Stats::statistic2pvalue (perm_distribution_pos, tfce_output_pos, pvalue_output_pos);
     Math::Stats::statistic2pvalue (perm_distribution_neg, tfce_output_neg, pvalue_output_neg);

     write_fixel_output (output_prefix + "_tfce_pos.msf", tfce_output_pos.data(), input_header, mask_vox, indexer_vox);
     write_fixel_output (output_prefix + "_tfce_neg.msf", tfce_output_neg.data(), input_header, mask_vox, indexer_vox);
     write_fixel_output (output_prefix + "_tvalue.msf", tvalue_output.data(), input_header, mask_vox, indexer_vox);
     write_fixel_output (output_prefix + "_pvalue_pos.msf", pvalue_output_pos.data(), input_header, mask_vox, indexer_vox);
     write_fixel_output (output_prefix + "_pvalue_neg.msf", pvalue_output_neg.data(), input_header, mask_vox, indexer_vox);
  }
}
