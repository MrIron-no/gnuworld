/**
 * EConfig.cc
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 *
 * $Id: EConfig.cc,v 1.9 2005/10/01 13:13:55 kewlio Exp $
 */

#include <unistd.h> // unlink()

#include <string>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <map>

#include <cstdlib>
#include <cstdio>  // rename()
#include <cstring> // strerror()
#include <cerrno>

#include "EConfig.h"
#include "StringTokenizer.h"
#include "logger.h"
#include "misc.h"

GNUWORLD_CORE_LOGGER(Config);

namespace gnuworld {

using std::endl;
using std::ifstream;
using std::map;
using std::ofstream;
using std::string;

EConfig::EConfig(const string& _configFileName) : configFileName(_configFileName), error(false) {
    openConfigFile();
}

EConfig::~EConfig() {
    // No heap space allocated
    valueMap.clear();
    lineList.clear();
}

EConfig::iterator EConfig::Find(const string& findMe) { return valueMap.find(findMe); }

EConfig::const_iterator EConfig::Find(const string& findMe) const { return valueMap.find(findMe); }

EConfig::iterator EConfig::Require(const string& key) {
    // Attempt to find the key in the map
    iterator ptr = valueMap.find(key);

    // Was it found?
    if (ptr == valueMap.end()) {
        // Nope, this method is intended to "require" the config file
        // to have a certain key/value pair.  Since this is not the
        // case, print out an error and quit.
        LOG(FATAL, "Configuration requires value for key \"{}\"", key);
        ::exit(0);
    }

    // At least one key/value pair for this key exists; return it
    return ptr;
}

bool EConfig::openConfigFile() {
    // elog	<< "EConfig> Opening configFile: "
    //	<< configFileName
    //	<< ", hasError(): "
    //	<< hasError()
    //	<< endl ;

    ifstream configFile(configFileName.c_str());
    if (!configFile.is_open()) {
        LOG(ERROR, "Unable to open file: {}", configFileName);
        setError();
        return false;
    }
    // else
    //	{
    //	elog	<< "EConfig> Successfully opened"
    //		<< endl ;
    //	}

    if (!readFile(configFile)) {
        setError();
        configFile.close();
        return false;
    }
    // else
    //	{
    //	elog	<< "EConfig> Successfully parsed"
    //		<< endl ;
    //	}
    configFile.close();
    return true;
}

bool EConfig::readFile(ifstream& configFile) {
    size_t lineNumber = 0;
    string tmp;

    while (getline(configFile, tmp)) {
        // Increment lineNumber before
        // any continue statements
        lineNumber++;

        if (!removeSpaces(tmp)) {
            // Parse error
            LOG(WARN, "Parse error at line: {} of {}", lineNumber, configFileName);
            return false;
        }

        if (tmp.empty() || '#' == tmp[0]) {
            lineInfo addMe(tmp);
            lineList.push_back(addMe);

            // Ignore this line
            continue;
        }

        // The line should now be of the form:
        // variablename=value
        StringTokenizer st(tmp, '=');

        if (st.size() < 2) {
            LOG(WARN, "Improper number of fields at line: {} of {}", lineNumber, configFileName);
            LOG(WARN, "Perhaps you meant for an empty value? Use: key = '' to specify empty "
                      "values");
            return false;
        }

        // Looks ok
        mapType::iterator mapItr = valueMap.insert(mapPairType(st[0], st.assemble(1)));

        lineList.push_back(lineInfo(st[0], st.assemble(1), mapItr));
    }

    return true;
}

bool EConfig::removeSpaces(string& line) {
    // If the first character is '#', just return true
    if (!line.empty() && ('#' == line[0])) {
        return true;
    }

    string::iterator ptr = line.begin();

    bool inValue = false;

    // Only continue up until the value field
    while (ptr != line.end()) {
        if (('=' == *ptr) && !inValue) {
            // We've reached the value field
            // Only remove comments from this point
            // on.
            if (((ptr + 1) != line.end()) && (*(ptr + 1) == ' ')) {
                ptr = line.erase(ptr + 1);
            }
            inValue = true;
        }

        // Remove any white space characters, so long
        // as we're not in the value field
        else if ((' ' == *ptr || '\t' == *ptr) && !inValue) {

            // erase() invalidates the iterator
            ptr = line.erase(ptr);

            continue;
        }

        else {
            ++ptr;
        }
    }

    // Remove trailing whitespace
    while (!line.empty() && ((' ' == line[line.size() - 1]) || ('\t' == line[line.size() - 1]))) {
        line.erase(line.size() - 1);
    }

    return true;
}

bool EConfig::Add(const string& key, const string& value) {
    // TODO: Add timestamp info?
    // Update the valueMap, which is used by clients of this class
    iterator mapItr = valueMap.insert(mapType::value_type(key, value));

    // Update the fileMap, which is used to keep track of the
    // format of the config file
    lineInfo addMe(key, value, mapItr);
    lineList.push_back(addMe);

    return writeFile();
}

bool EConfig::writeFile() {
    // Move the old file to the /tmp directory to ensure we have
    // a backup.
    string backupFileName(configFileName);

    // Remove any leading directory path information
    string::size_type slashPos = backupFileName.find_last_of('/');
    if (string::npos != slashPos) {
        // There is leading path information
        backupFileName = backupFileName.substr(slashPos, string::npos);
    }
    backupFileName = string("/tmp/") + backupFileName;

    // elog	<< "EConfig::writeFile> Renaming "
    //	<< configFileName
    //	<< " to "
    //	<< backupFileName
    //	<< endl ;

    if (::rename(configFileName.c_str(), backupFileName.c_str()) < 0) {
        LOG(ERROR, "Unable to rename {} to {} because: {}", configFileName, backupFileName,
            std::string(strerror(errno)));

        // Ok to leave the file open per class conditions.
        return false;
    }

    std::ofstream configFile(configFileName.c_str());
    if (!configFile.is_open()) {
        LOG(ERROR, "Unable to open file: {}, because: {}", configFileName,
            std::string(strerror(errno)));

        if (::rename(backupFileName.c_str(), configFileName.c_str()) < 0) {
            LOG(ERROR, "Unable to restore original file \"{}\" from backup \"{}\" because: {}",
                configFileName, backupFileName, std::string(strerror(errno)));
        }
        return false;
    }

    for (lineListType::const_iterator fileItr = lineList.begin(); fileItr != lineList.end();
         ++fileItr) {
        const lineInfo& theInfo = *fileItr;
        if (theInfo.value.empty()) {
            //		elog	<< "EConfig::writeFile> Writing: "
            //			<< theInfo.key
            //			<< endl ;

            // comment line
            configFile << theInfo.key << endl;

            continue;
        }

        //	elog	<< "EConfig::writeFile> Writing: "
        //		<< theInfo.key
        //		<< " = "
        //		<< theInfo.value
        //		<< endl ;

        configFile << theInfo.key << " = " << theInfo.value << endl;
    } // for()

    configFile.close();

    // Writing of new file succeeded, remove the backup
    if (::unlink(backupFileName.c_str()) < 0) {
        LOG(WARN, "Unable to unlink backup file {} because: {}", backupFileName,
            std::string(strerror(errno)));

        // This is not a critical failure
        // Allow the method to return true
    }

    return true;
} // writeFile()

bool EConfig::Delete(const string& key) {
    iterator itr = valueMap.find(key);
    if (itr == valueMap.end()) {
        return false;
    }
    return Delete(itr);
}

bool EConfig::Delete(iterator itr) {
    if (itr == valueMap.end()) {
        LOG(WARN, "Attempting to delete valueMap.end()");
        return false;
    }
    valueMap.erase(itr);

    bool foundIt = false;
    for (lineListType::iterator lineItr = lineList.begin(); lineItr != lineList.end(); ++lineItr) {
        if (itr == (*lineItr).mapItr) {
            // Found it
            foundIt = true;
            lineList.erase(lineItr);

            break;
        }
    }

    if (!foundIt) {
        LOG(WARN, "Unable to find lineList entry corresponding to iterator: {}/{}", itr->first,
            itr->second);
    }

    return (foundIt ? writeFile() : false);
}

bool EConfig::Replace(iterator itr, const string& newValue) {
    if (newValue.empty()) {
        return false;
    }

    if (itr == valueMap.end()) {
        return false;
    }
    valueMap.erase(itr);

    bool foundIt = false;

    // Replacing the value in-place here should maintain the
    // relative location of the key/value pair in the file.
    for (lineListType::iterator listItr = lineList.begin(); listItr != lineList.end(); ++listItr) {
        if ((*listItr).mapItr == itr) {
            // Found it
            foundIt = true;
            (*listItr).value = newValue;
            break;
        }
    }

    if (!foundIt) {
        LOG(WARN, "Unable to find lineList entry corresponding to iterator: {}/{}", itr->first,
            itr->second);
    }

    valueMap.insert(mapType::value_type(itr->first, newValue));

    return writeFile();
}

void EConfig::Clear() {
    error = false;
    configErrors.clear();
    valueMap.clear();
}

bool EConfig::AddComment(const string& commentData) {
    string newComment(commentData);

    // If this command is not empty and does not begin with the
    // proper comment delimiter, then add the delimiter.
    if (!newComment.empty() && ('#' != newComment[0])) {
        newComment = string("# ") + newComment;
    }

    // For some reason the compiler won't let me use the 3 argument
    // constructor for lineInfo here -- dan
    lineInfo addMe;
    addMe.key = newComment;
    lineList.push_back(addMe);

    return writeFile();
}

} // namespace gnuworld
