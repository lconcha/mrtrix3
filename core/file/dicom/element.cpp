/* Copyright (c) 2008-2017 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * MRtrix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/.
 */


#include "file/path.h"
#include "file/dicom/element.h"
#include "debug.h"

namespace MR {
  namespace File {
    namespace Dicom {

      const char* Element::error_message = nullptr;

      void Element::set (const std::string& filename, bool force_read, bool read_write)
      {
        group = element = VR = 0;
        size = 0;
        start = data = next = NULL;
        is_BE = is_transfer_syntax_BE = false;
        parents.clear();

        fmap.reset (new File::MMap (filename, read_write));

        if (fmap->size() < 256)
          throw Exception ("\"" + fmap->name() + "\" is too small to be a valid DICOM file");

        next = fmap->address();

        if (memcmp (next + 128, "DICM", 4)) {
          is_explicit = false;
          DEBUG ("DICOM magic number not found in file \"" + fmap->name() + "\" - trying truncated format");
          if (!force_read)
            if (!Path::has_suffix (fmap->name(), ".dcm"))
              throw Exception ("file \"" + fmap->name() + "\" does not have the DICOM magic number or the .dcm extension - assuming not DICOM");
        }
        else next += 132;

        try { set_explicit_encoding(); }
        catch (Exception) {
          throw Exception ("\"" + fmap->name() + "\" is not a valid DICOM file");
          fmap.reset();
        }
      }






      void Element::set_explicit_encoding ()
      {
        assert (fmap);
        if (read_GR_EL())
          throw Exception ("\"" + fmap->name() + "\" is too small to be DICOM");

        is_explicit = true;
        next = start;
        VR = ByteOrder::BE (*reinterpret_cast<uint16_t*> (start+4));

        if ((VR == VR_OB) | (VR == VR_OW) | (VR == VR_OF) | (VR == VR_SQ) |
            (VR == VR_UN) | (VR == VR_AE) | (VR == VR_AS) | (VR == VR_AT) |
            (VR == VR_CS) | (VR == VR_DA) | (VR == VR_DS) | (VR == VR_DT) |
            (VR == VR_FD) | (VR == VR_FL) | (VR == VR_IS) | (VR == VR_LO) |
            (VR == VR_LT) | (VR == VR_PN) | (VR == VR_SH) | (VR == VR_SL) |
            (VR == VR_SS) | (VR == VR_ST) | (VR == VR_TM) | (VR == VR_UI) |
            (VR == VR_UL) | (VR == VR_US) | (VR == VR_UT)) return;

        DEBUG ("using implicit DICOM encoding");
        is_explicit = false;
      }






      bool Element::read_GR_EL ()
      {
        group = element = VR = 0;
        size = 0;
        start = next;
        data = next = NULL;

        if (start < fmap->address())
          throw Exception ("invalid DICOM element");

        if (start + 8 > fmap->address() + fmap->size())
          return true;

        is_BE = is_transfer_syntax_BE;

        group = Raw::fetch_<uint16_t> (start, is_BE);

        if (group == GROUP_BYTE_ORDER_SWAPPED) {
          if (!is_BE)
            throw Exception ("invalid DICOM group ID " + str (group) + " in file \"" + fmap->name() + "\"");

          is_BE = false;
          group = GROUP_BYTE_ORDER;
        }
        element = Raw::fetch_<uint16_t> (start+2, is_BE);

        return false;
      }






      bool Element::read ()
      {
        if (read_GR_EL())
          return false;

        data = start + 8;
        if ((is_explicit && group != GROUP_SEQUENCE) || group == GROUP_BYTE_ORDER) {

          // explicit encoding:
          VR = ByteOrder::BE (*reinterpret_cast<uint16_t*> (start+4));
          if (VR == VR_OB || VR == VR_OW || VR == VR_OF || VR == VR_SQ || VR == VR_UN || VR == VR_UT) {
            size = Raw::fetch_<uint32_t> (start+8, is_BE);
            data += 4;
          }
          else size = Raw::fetch_<uint16_t> (start+6, is_BE);

          // try figuring out VR from dictionary if vendors haven't bothered
          // filling it in...
          if (VR == VR_UN) {
            std::string name = tag_name();
            if (name.size())
              VR = get_VR_from_tag_name (name);
          }
        }
        else {

          // implicit encoding:
          std::string name = tag_name();
          if (!name.size()) {
            DEBUG (printf ("WARNING: unknown DICOM tag (%02X %02X) "
                  "with implicit encoding in file \"", group, element)
                + fmap->name() + "\"");
            VR = VR_UN;
          }
          else
            VR = get_VR_from_tag_name (name);
          size = Raw::fetch_<uint32_t> (start+4, is_BE);
        }


        next = data;
        if (size == LENGTH_UNDEFINED) {
          if (VR != VR_SQ && !(group == GROUP_SEQUENCE && element == ELEMENT_SEQUENCE_ITEM)) {
            INFO ("undefined length used for DICOM tag " + ( tag_name().size() ? tag_name().substr (2) : "" )
                + MR::printf ("(%04X, %04X) in file \"", group, element) + fmap->name() + "\"");
            size = 0;
          }
        }
        else if (next+size > fmap->address() + fmap->size())
          throw Exception ("file \"" + fmap->name() + "\" is too small to contain DICOM elements specified");
        else {
          if (size%2)
            DEBUG ("WARNING: odd length (" + str (size) + ") used for DICOM tag " + ( tag_name().size() ? tag_name().substr (2) : "" )
                + " (" + str (group) + ", " + str (element) + ") in file \"" + fmap->name() + "");
          if (VR != VR_SQ && ( group != GROUP_SEQUENCE || element != ELEMENT_SEQUENCE_ITEM ) )
            next += size;
        }



        if (parents.size())
          if ((parents.back().end && data > parents.back().end) ||
              (group == GROUP_SEQUENCE && element == ELEMENT_SEQUENCE_DELIMITATION_ITEM))
            parents.pop_back();

        if (VR == VR_SQ) {
          if (size == LENGTH_UNDEFINED)
            parents.push_back (Sequence (group, element, NULL));
          else
            parents.push_back (Sequence (group, element, data + size));
        }




        switch (group) {
          case GROUP_BYTE_ORDER:
            switch (element) {
              case ELEMENT_TRANSFER_SYNTAX_UID:
                if (strncmp (reinterpret_cast<const char*> (data), "1.2.840.10008.1.2.1", size) == 0) {
                  is_BE = is_transfer_syntax_BE = false; // explicit VR Little Endian
                  is_explicit = true;
                }
                else if (strncmp (reinterpret_cast<const char*> (data), "1.2.840.10008.1.2.2", size) == 0) {
                  is_BE = is_transfer_syntax_BE = true; // Explicit VR Big Endian
                  is_explicit = true;
                }
                else if (strncmp (reinterpret_cast<const char*> (data), "1.2.840.10008.1.2", size) == 0) {
                  is_BE = is_transfer_syntax_BE = false; // Implicit VR Little Endian
                  is_explicit = false;
                }
                else if (strncmp (reinterpret_cast<const char*> (data), "1.2.840.10008.1.2.1.99", size) == 0) {
                  throw Exception ("DICOM deflated explicit VR little endian transfer syntax not supported");
                }
                else {
                  error_message =
                    "unsupported transfer syntax found in DICOM data\n"
                    "consider using third-party tools to convert your data to standard uncompressed encoding\n"
                    "e.g. dcmtk: http://dicom.offis.de/dcmtk.php.en";
                  INFO ("unsupported DICOM transfer syntax: \"" + std::string (reinterpret_cast<const char*> (data), size)
                    + "\" in file \"" + fmap->name() + "\"");
                }
                break;
            }

            break;
        }

        return true;
      }












      Element::Type Element::type () const
      {
        if (!VR) return INVALID;
        if (VR == VR_FD || VR == VR_FL) return FLOAT;
        if (VR == VR_SL || VR == VR_SS) return INT;
        if (VR == VR_UL || VR == VR_US) return UINT;
        if (VR == VR_SQ) return SEQ;
        if (VR == VR_AE || VR == VR_AS || VR == VR_CS || VR == VR_DA ||
            VR == VR_DS || VR == VR_DT || VR == VR_IS || VR == VR_LO ||
            VR == VR_LT || VR == VR_PN || VR == VR_SH || VR == VR_ST ||
            VR == VR_TM || VR == VR_UI || VR == VR_UT || VR == VR_AT) return STRING;
        return OTHER;
      }



      vector<int32_t> Element::get_int () const
      {
        vector<int32_t> V;
        if (VR == VR_SL)
          for (const uint8_t* p = data; p < data + size; p += sizeof (int32_t))
            V.push_back (Raw::fetch_<int32_t> (p, is_BE));
        else if (VR == VR_SS)
          for (const uint8_t* p = data; p < data + size; p += sizeof (int16_t))
            V.push_back (Raw::fetch_<int16_t> (p, is_BE));
        else if (VR == VR_IS) {
          vector<std::string> strings (split (std::string (reinterpret_cast<const char*> (data), size), "\\", false));
          V.resize (strings.size());
          for (size_t n = 0; n < V.size(); n++)
            V[n] = to<int32_t> (strings[n]);
        }
        else
          report_unknown_tag_with_implicit_syntax();

        return V;
      }




      vector<uint32_t> Element::get_uint () const
      {
        vector<uint32_t> V;
        if (VR == VR_UL)
          for (const uint8_t* p = data; p < data + size; p += sizeof (uint32_t))
            V.push_back (Raw::fetch_<uint32_t> (p, is_BE));
        else if (VR == VR_US)
          for (const uint8_t* p = data; p < data + size; p += sizeof (uint16_t))
            V.push_back (Raw::fetch_<uint16_t> (p, is_BE));
        else if (VR == VR_IS) {
          vector<std::string> strings (split (std::string (reinterpret_cast<const char*> (data), size), "\\", false));
          V.resize (strings.size());
          for (size_t n = 0; n < V.size(); n++) V[n] = to<uint32_t> (strings[n]);
        }
        else
          report_unknown_tag_with_implicit_syntax();
        return V;
      }



      vector<double> Element::get_float () const
      {
        vector<double> V;
        if (VR == VR_FD)
          for (const uint8_t* p = data; p < data + size; p += sizeof (float64))
            V.push_back (Raw::fetch_<float64> (p, is_BE));
        else if (VR == VR_FL)
          for (const uint8_t* p = data; p < data + size; p += sizeof (float32))
            V.push_back (Raw::fetch_<float32> (p, is_BE));
        else if (VR == VR_DS || VR == VR_IS) {
          vector<std::string> strings (split (std::string (reinterpret_cast<const char*> (data), size), "\\", false));
          V.resize (strings.size());
          for (size_t n = 0; n < V.size(); n++)
            V[n] = to<double> (strings[n]);
        }
        else
          report_unknown_tag_with_implicit_syntax();
        return V;
      }





      vector<std::string> Element::get_string () const
      {
        if (VR == VR_AT) {
          vector<std::string> strings;
          strings.push_back (printf ("%02X %02X", Raw::fetch_<uint16_t> (data, is_BE), Raw::fetch_<uint16_t> (data+2, is_BE)));
          return strings;
        }

        vector<std::string> strings (split (std::string (reinterpret_cast<const char*> (data), size), "\\", false));
        for (vector<std::string>::iterator i = strings.begin(); i != strings.end(); ++i) {
          *i = strip (*i);
          replace (*i, '^', ' ');
        }
        return strings;
      }



      namespace {
        template <class T>
          inline void print_vec (const vector<T>& V)
          {
            for (size_t n = 0; n < V.size(); n++)
              fprintf (stdout, "%s ", str (V[n]).c_str());
          }
      }











      std::ostream& operator<< (std::ostream& stream, const Element& item)
      {
        //return "TYPE  GROUP ELEMENT VR  SIZE  OFFSET  NAME                               CONTENTS";

        const std::string& name (item.tag_name());
        stream << printf ("[DCM] %04X %04X %c%c % 8u % 8llu ", item.group, item.element,
            reinterpret_cast<const char*> (&item.VR)[1], reinterpret_cast<const char*> (&item.VR)[0],
            ( item.size == LENGTH_UNDEFINED ? uint32_t(0) : item.size ), item.offset (item.start));

        std::string tmp;
        size_t indent = item.level() - ( item.VR == VR_SQ ? 1 : 0 );
        for (size_t i = 0; i < indent; i++)
          tmp += "  ";
        if (item.VR == VR_SQ)
          tmp += "> ";
        else if (item.group == GROUP_SEQUENCE && item.element == ELEMENT_SEQUENCE_ITEM)
          tmp += "- ";
        else
          tmp += "  ";
        tmp += ( name.size() ? name.substr(2) : "unknown" );
        tmp.resize (40, ' ');
        stream << tmp + ' ';

        switch (item.type()) {
          case Element::INT:
            stream << item.get_int();
            break;
          case Element::UINT:
            stream << item.get_uint();
            break;
          case Element::FLOAT:
            stream << item.get_float();
            break;
          case Element::STRING:
            if (item.group == GROUP_DATA && item.element == ELEMENT_DATA)
              stream << "(data)";
            else
              stream << item.get_string();
            break;
          case Element::SEQ:
            break;
          default:
            if (item.group != GROUP_SEQUENCE || item.element != ELEMENT_SEQUENCE_ITEM)
              stream << "unknown data type";
        }

        stream << "\n";

        return stream;
      }


    }
  }
}

