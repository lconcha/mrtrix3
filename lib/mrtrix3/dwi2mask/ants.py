# Copyright (c) 2008-2019 the MRtrix3 contributors.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Covered Software is provided under this License on an "as is"
# basis, without warranty of any kind, either expressed, implied, or
# statutory, including, without limitation, warranties that the
# Covered Software is free of defects, merchantable, fit for a
# particular purpose or non-infringing.
# See the Mozilla Public License v. 2.0 for more details.
#
# For more details, see http://www.mrtrix.org/.

import os
from distutils.spawn import find_executable
from mrtrix3 import MRtrixError
from mrtrix3 import app, fsl, image, path, run

def usage(base_parser, subparsers): #pylint: disable=unused-variable
  parser = subparsers.add_parser('ants', parents=[base_parser])
  parser.set_author('Robert E. Smith (robert.smith@florey.edu.au)')
  parser.set_synopsis('Use ANTs Brain Extraction to derive a DWI brain mask')
  parser.add_citation('B. Avants, N.J. Tustison, G. Song, P.A. Cook, A. Klein, J.C. Jee. A reproducible evaluation of ANTs similarity metric performance in brain image registration. NeuroImage, 2011, 54, 2033-2044', is_external=True)
  parser.add_argument('input',  help='The input DWI series')
  parser.add_argument('output', help='The output mask image')
  options = parser.add_argument_group('Options specific to the "ants" algorithm')
  options.add_argument('-template', metavar='TemplateImage MaskImage', nargs=2, help='Provide the template image and corresponding mask for antsBrainExtraction.sh to use; the template image should be T2-weighted.')



def get_inputs(): #pylint: disable=unused-variable
  if not app.ARGS.template:
    raise MRtrixError('For "ants" dwi2mask algorithm, '
                      '-template command-line option is currently mandatory')
  run.command('mrconvert ' + app.ARGS.template[0] + ' ' + path.to_scratch('template_image.nii')
              + ' -strides +1,+2,+3')
  run.command('mrconvert ' + app.ARGS.template[1] + ' ' + path.to_scratch('template_mask.nii')
              + ' -strides +1,+2,+3')



def execute(): #pylint: disable=unused-variable
  ants_path = os.environ.get('ANTSPATH', '')
  if not ants_path:
    raise MRtrixError('Environment variable ANTSPATH is not set; '
                      'please appropriately confirure ANTs software')
  ants_brain_extraction_cmd = find_executable('antsBrainExtraction.sh')
  if not ants_brain_extraction_cmd:
    raise MRtrixError('Unable to find command "'
                      + ants_brain_extraction_cmd
                      + '"; please check ANTs installation')

  # Produce mean b=0 image
  run.command('dwiextract input.mif -bzero - | '
              'mrmath - mean - -axis 3 | '
              'mrconvert - bzero.nii -strides +1,+2,+3')

  run.command('antsBrainExtraction.sh '
              + ' -d 3'
              + ' -c 3x3x2x1'
              + ' -a bzero.nii'
              + ' -e ' + app.ARGS.template[0]
              + ' -m ' + app.ARGS.template[1]
              + ' -o out'
              + ('' if app.DO_CLEANUP else ' -k 1')
              + (' -z' if app.VERBOSITY >= 3 else ''))

  strides = image.Header('input.mif').strides()[0:3]
  strides = [(abs(value) + 1 - min(abs(v) for v in strides)) * (-1 if value < 0 else 1) for value in strides]

  run.command('mrconvert outBrainExtractionMask.nii.gz '
              + path.from_user(app.ARGS.output)
              + ' -strides ' + ','.join(str(value) for value in strides),
              mrconvert_keyval=path.from_user(app.ARGS.input, False),
              force=app.FORCE_OVERWRITE)
