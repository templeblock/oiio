/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>

#include "dassert.h"
#include "plugin.h"
#include "strutil.h"
#include "filesystem.h"

#define DLL_EXPORT_PUBLIC /* Because we are implementing ImageIO */
#include "imageio.h"
#include "imageio_pvt.h"
#undef DLL_EXPORT_PUBLIC

using namespace OpenImageIO;
using namespace OpenImageIO::pvt;



// Map format name to ImageInput creation
static std::map <std::string, create_prototype> input_formats;
// Map format name to ImageOutput creation
static std::map <std::string, create_prototype> output_formats;
// Map file extension to ImageInput creation
static std::map <std::string, create_prototype> input_extensions;
// Map file extension to ImageOutput creation
static std::map <std::string, create_prototype> output_extensions;
// Map format name to plugin handle
static std::map <std::string, Plugin::Handle> plugin_handles;
// Map format name to full path
static std::map <std::string, std::string> plugin_filepaths;

// FIXME -- do we use the extensions above?


static std::string pattern = Strutil::format (".imageio.%s",
                                              Plugin::plugin_extension());



static void
catalog_plugin (const std::string &format_name,
                const std::string &plugin_fullpath)
{
    // Remember the plugin
    std::map<std::string, std::string>::const_iterator found_path;
    found_path = plugin_filepaths.find (format_name);
    if (found_path != plugin_filepaths.end()) {
        // Hey, we already have an entry for this format
        if (found_path->second == plugin_fullpath) {
            // It's ok if they're both the same file; just skip it.
            return;
        }
        // if (verbosity > 1)
        std::cerr << "ImageIO WARNING: " << format_name << " had multiple plugins:\n"
                  << "\t\"" << found_path->second << "\"\n"
                  << "    as well as\n"
                  << "\t\"" << plugin_fullpath << "\"\n"
                  << "    Ignoring all but the first one.\n";
        return;
    }

    Plugin::Handle handle = Plugin::open (plugin_fullpath);
    if (! handle) {
        // If verbosity > 1
        // std::cerr << "Open of " << plugin_fullpath << " failed:\n" 
        //          << Plugin::error_message() << "\n";
        return;
    }
//        if (verbosity > 1)
//    std::cerr << "Succeeded in opening " << plugin_fullpath << "\n";
    
    int *plugin_version = (int *) Plugin::getsym (handle, "imageio_version");
    if (! plugin_version || *plugin_version != IMAGEIO_VERSION) {
        // OpenImageIO::error ("Plugin \"%s\" did not have 'imageio_version' symbol",
        //                    plugin_filename.c_str());
        Plugin::close (handle);
        return;
    }

    // Add the filepath and handle to the master lists
    plugin_filepaths[format_name] = plugin_fullpath;
    plugin_handles[format_name] = handle;

    // Look for output creator and list of supported extensions
    bool useful = false;
    std::string create_name = format_name + "_output_imageio_create";
    create_prototype create_function = 
        (create_prototype) Plugin::getsym (handle, create_name);
    if (create_function) {
        useful = true;
        output_formats[format_name] = create_function;
        std::string extsym = format_name + "_output_extensions";
        for (char **e = (char **)Plugin::getsym(handle, extsym); e && *e; ++e) {
            std::string ext (*e);
            boost::algorithm::to_lower (ext);
            // std::cerr << "  output extension " << ext << "\n";
            if (output_formats.find(ext) == output_formats.end())
                output_formats[ext] = create_function;
        }
    }

    // Look for input creator and list of supported extensions
    create_name = format_name + "_input_imageio_create";
    create_function = (create_prototype) Plugin::getsym (handle, create_name);
    if (create_function) {
        useful = true;
        input_formats[format_name] = create_function;
        std::string extsym = format_name + "_input_extensions";
        for (char **e = (char **)Plugin::getsym(handle, extsym); e && *e; ++e) {
            std::string ext (*e);
            boost::algorithm::to_lower (ext);
            // std::cerr << "  input extension " << ext << "\n";
            if (input_formats.find(ext) == input_formats.end())
                input_formats[ext] = create_function;
        }
    }

    // If we found neither input nor output creation functions, close the
    // plugin.
    if (! useful)
        Plugin::close (handle);
}



/// Look at ALL imageio plugins in the searchpath and add them to the
/// catalog.  This routine is not reentrant and should only be called
/// by a routine that is holding a lock on imageio_mutex.
static void
catalog_all_plugins (std::string searchpath)
{
    const char *imageio_library_path = getenv ("IMAGEIO_LIBRARY_PATH");
    if (imageio_library_path && *imageio_library_path) {
        std::string newpath = imageio_library_path;
        if (searchpath.length())
            newpath = newpath + ':' + searchpath;
        searchpath = newpath;
    }
//    std::cerr << "catalog_all_plugins: searchpath = '" << searchpath << "'\n";

    size_t patlen = pattern.length();
//    std::cerr << "pattern is " << pattern << ", length=" << patlen << "\n";
    std::vector<std::string> dirs;
    Filesystem::searchpath_split (searchpath, dirs, true);
    BOOST_FOREACH (std::string &dir, dirs) {
//        std::cerr << "Directory " << dir << "\n";
        boost::filesystem::directory_iterator end_itr; // default construction yields past-the-end
        for (boost::filesystem::directory_iterator itr (dir);
              itr != end_itr;  ++itr) {
            std::string full_filename = itr->path().string();
            std::string leaf = itr->path().leaf();
//            std::cerr << "\tfound file " << full_filename << ", leaf = '" << leaf << "'\n";
            size_t found = leaf.find (pattern);
            if (found != std::string::npos &&
                (found == leaf.length() - patlen)) {
                std::string pluginname (leaf.begin(), leaf.begin() + leaf.length() - patlen);
//                std::cerr << "\t\tFound imageio plugin " << full_filename << "\n";
//                std::cerr << "\t\t\tplugin name = '" << pluginname << "'\n";
                catalog_plugin (pluginname, full_filename);
//                plugin_names.push_back (full_filename);
//                plugin_handles.push_back (0);
            }
        }
    }
//    std::cerr << "done catalog_all\n";
}



ImageOutput *
ImageOutput::create (const std::string &filename, const std::string &plugin_searchpath)
{
    if (filename.empty()) { // Can't even guess if no filename given
        OpenImageIO::error ("ImageOutput::create() called with no filename");
        return NULL;
    }

    // Extract the file extension from the filename
    std::string format = boost::filesystem::extension (filename);
    if (format.empty()) {
        // If the file had no extension, maybe it was itself the format name
        format = filename;
    } else {
        if (format[0] == '.')
            format.erase (format.begin());  // Erase leading dot
        // if (verbose > 1)
        // std::cerr << "extension of '" << filename << "' is '" << format << "'\n";
    }

    recursive_lock_guard lock (imageio_mutex);  // Ensure thread safety

    // See if it's already in the table.  If not, scan all plugins we can
    // find to populate the table.
    boost::algorithm::to_lower (format);
    if (output_formats.find (format) == output_formats.end())
        catalog_all_plugins (plugin_searchpath);

    if (output_formats.find (format) == output_formats.end()) {
        OpenImageIO::error ("ImageOutput::create_format() could not find a plugin for \"%s\"\n    searchpath = \"%s\"\n",
                            filename.c_str(), plugin_searchpath.c_str());
        return NULL;
    }

    create_prototype create_function = output_formats[format];
    ASSERT (create_function != NULL);
    return (ImageOutput *) create_function();
}



ImageInput *
ImageInput::create (const std::string &filename, const std::string &plugin_searchpath)
{
    if (filename.empty()) { // Can't even guess if no filename given
        OpenImageIO::error ("ImageInput::create() called with no filename");
        return NULL;
    }

    // Extract the file extension from the filename
    std::string format = boost::filesystem::extension (filename);
    if (format.empty()) {
        // If the file had no extension, maybe it was itself the format name
        format = filename;
    } else {
        if (format[0] == '.')
            format.erase (format.begin());  // Erase leading dot
        // if (verbose > 1)
        // std::cerr << "extension of '" << filename << "' is '" << format << "'\n";
    }

    recursive_lock_guard lock (imageio_mutex);  // Ensure thread safety

    // See if it's already in the table.  If not, scan all plugins we can
    // find to populate the table.
    boost::algorithm::to_lower (format);
    if (input_formats.find (format) == input_formats.end())
        catalog_all_plugins (plugin_searchpath);

    if (input_formats.find (format) == input_formats.end()) {
        OpenImageIO::error ("ImageInput::create_format() could not find a plugin for \"%s\"\n    searchpath = \"%s\"\n",
                            filename.c_str(), plugin_searchpath.c_str());
        return NULL;
    }

    // FIXME: if a plugin can't be found that was explicitly designated
    // for this extension, then just try every one we find and see if
    // any will open the file.

    create_prototype create_function = input_formats[format];
    ASSERT (create_function != NULL);
    return (ImageInput *) create_function();
}
