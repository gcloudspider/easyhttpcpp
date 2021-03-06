/*
 * Copyright 2017 Sony Corporation
 */

#ifndef EASYHTTPCPP_TESTUTIL_TESTDATABASEUTIL_H_INCLUDED
#define EASYHTTPCPP_TESTUTIL_TESTDATABASEUTIL_H_INCLUDED

#include "Poco/Path.h"

namespace easyhttpcpp {
namespace testutil {

class TestDatabaseUtil {
public:
    static bool isTableExist(const Poco::Path& databasePath, unsigned int version, const std::string& tableName);
private:
    TestDatabaseUtil();
};

} /* namespace testutil */
} /* namespace easyhttpcpp */

#endif /* EASYHTTPCPP_TESTUTIL_TESTDATABASEUTIL_H_INCLUDED */
