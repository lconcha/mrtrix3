/*
    Copyright 2009 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 26/08/09.

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

#include "file/misc.h"
#include "file/path.h"
#include "file/gz.h"
#include "image/misc.h"
#include "image/header.h"
#include "image/handler/gz.h"
#include "image/format/list.h"
#include "image/format/nifti1_utils.h"

namespace MR {
  namespace Image {
    namespace Format {

      
      bool NIfTI_GZ::read (Header& H) const
      {
        if (!Path::has_suffix (H.name(), ".nii.gz")) return (false);

        nifti_1_header NH;
        File::GZ zf (H.name(), "rb");
        zf.read (reinterpret_cast<char*> (&NH), 352);
        zf.close();
       
        size_t data_offset = File::NIfTI::read (H, NH);

        H.handler = new Handler::GZ (H, 0, false);
        H.files.push_back (File::Entry (H.name(), data_offset));

        return (true);
      }





      bool NIfTI_GZ::check (Header& H, int num_axes) const
      {
        if (!Path::has_suffix (H.name(), ".nii.gz")) return (false);
        if (num_axes < 3) throw Exception ("cannot create NIfTI-1.1 image with less than 3 dimensions");
        if (num_axes > 8) throw Exception ("cannot create NIfTI-1.1 image with more than 8 dimensions");

        H.axes.ndim() = num_axes;
        for (size_t i = 0; i < H.ndim(); i++) {
          if (H.axes.dim(i) < 1) H.axes.dim(i) = 1;
          H.axes.order(i) = i;
          H.axes.forward(i) = true;
        }

        H.axes.description(0) = Axes::left_to_right;
        H.axes.units(0) = Axes::millimeters;

        H.axes.description(1) = Axes::posterior_to_anterior;
        H.axes.units(1) = Axes::millimeters;

        H.axes.description(1) = Axes::inferior_to_superior;
        H.axes.units(1) = Axes::millimeters;

        return (true);
      }





      void NIfTI_GZ::create (Header& H) const
      {
        if (H.ndim() > 7) 
          throw Exception ("NIfTI-1.1 format cannot support more than 7 dimensions for image \"" + H.name() + "\"");

        Handler::GZ* handler = new Handler::GZ (H, 352, true);
      
        File::NIfTI::write (*reinterpret_cast<nifti_1_header*> (handler->header()), H);

        H.handler = handler;

        File::create (H.name());
        H.files.push_back (File::Entry (H.name(), 352));
      }

    }
  }
}

