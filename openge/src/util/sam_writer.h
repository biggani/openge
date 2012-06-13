/*********************************************************************
 *
 * sam_writer.h: A simple SAM writer.
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 6 June 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial 
 * Purpose License. A copy of this license has been provided in 
 * the openge/ directory.
 *
 *********************************************************************/

#ifndef OGE_SAMREADER_H
#define OGE_SAMREADER_H
#include <string>
#include <iostream>
#include <api/BamAlignment.h>
#include <api/SamHeader.h>

// SamReader is capable of sequentially reading a SAM file. It doesn't support
// most of the features that BamReader does, only enough to support converting SAM
// files to the BAM format.
class SamWriter
{
public:
    SamWriter();
    bool Open(const std::string& filename,
              const std::string& samHeaderText,
              const BamTools::RefVector& referenceSequences);
    bool Close();
    bool SaveAlignment(const BamTools::BamAlignment & al);
protected:
    std::ofstream file;
    BamTools::SamHeader header;
    BamTools::RefVector references;
    std::string filename;

    bool open;
};

#endif