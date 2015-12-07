/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <pbxbuild/Tool/SearchPaths.h>
#include <pbxbuild/TypeResolvedFile.h>
#include <pbxbuild/HeaderMap.h>

using pbxbuild::Tool::SearchPaths;
using libutil::FSUtil;

SearchPaths::
SearchPaths(std::vector<std::string> const &headerSearchPaths, std::vector<std::string> const &userHeaderSearchPaths, std::vector<std::string> const &frameworkSearchPaths, std::vector<std::string> const &librarySearchPaths) :
    _headerSearchPaths    (headerSearchPaths),
    _userHeaderSearchPaths(userHeaderSearchPaths),
    _frameworkSearchPaths (frameworkSearchPaths),
    _librarySearchPaths   (librarySearchPaths)
{
}

SearchPaths::
~SearchPaths()
{
}

static void
AppendPaths(std::vector<std::string> *args, std::string const &working, std::vector<std::string> const &paths)
{
    for (std::string const &path : paths) {
        std::string recursive = "**";
        if (path.size() >= recursive.size() && path.substr(path.size() - recursive.size()) == recursive) {
            std::string root = path.substr(0, path.size() - recursive.size());
            args->push_back(root);

            FSUtil::EnumerateRecursive(working + "/" + root, [&](std::string const &path) -> bool {
                // TODO(grp): Use build settings for included and excluded recursive paths.

                if (FSUtil::TestForDirectory(path)) {
                    args->push_back(path.substr(root.size()));
                }
                return true;
            });
        } else {
            args->push_back(path);
        }
    }
}

std::vector<std::string> SearchPaths::
ExpandRecursive(pbxsetting::Environment const &environment, std::vector<std::string> const &paths, std::string const &workingDirectory)
{
    std::vector<std::string> result;
    AppendPaths(&result, workingDirectory, paths);
    return result;
}

SearchPaths SearchPaths::
Create(
    std::string const &workingDirectory,
    pbxsetting::Environment const &environment
)
{
    std::vector<std::string> headerSearchPaths;
    AppendPaths(&headerSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("PRODUCT_TYPE_HEADER_SEARCH_PATHS")));
    AppendPaths(&headerSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("HEADER_SEARCH_PATHS")));

    std::vector<std::string> userHeaderSearchPaths;
    AppendPaths(&userHeaderSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("USER_HEADER_SEARCH_PATHS")));

    std::vector<std::string> frameworkSearchPaths;
    AppendPaths(&frameworkSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("FRAMEWORK_SEARCH_PATHS")));
    AppendPaths(&frameworkSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("PRODUCT_TYPE_FRAMEWORK_SEARCH_PATHS")));

    std::vector<std::string> librarySearchPaths;
    AppendPaths(&frameworkSearchPaths, workingDirectory, pbxsetting::Type::ParseList(environment.resolve("LIBRARY_SEARCH_PATHS")));

    return SearchPaths(
        headerSearchPaths,
        userHeaderSearchPaths,
        frameworkSearchPaths,
        librarySearchPaths
    );
}
