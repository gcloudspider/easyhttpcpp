/*
 * Copyright 2017 Sony Corporation
 */

#include "gtest/gtest.h"

#include "Poco/Buffer.h"
#include "Poco/File.h"
#include "Poco/FileStream.h"
#include "Poco/SharedPtr.h"
#include "Poco/Net/HTTPResponse.h"

#include "easyhttpcpp/common/CacheMetadata.h"
#include "easyhttpcpp/common/CommonMacros.h"
#include "easyhttpcpp/common/FileUtil.h"
#include "easyhttpcpp/common/StringUtil.h"
#include "easyhttpcpp/Headers.h"
#include "HeadersEqualMatcher.h"
#include "TestFileUtil.h"

#include "HttpIntegrationTestCase.h"
#include "HttpCacheMetadata.h"
#include "HttpFileCache.h"
#include "HttpTestConstants.h"
#include "HttpTestUtil.h"
#include "HttpUtil.h"

using easyhttpcpp::common::CacheMetadata;
using easyhttpcpp::common::FileUtil;
using easyhttpcpp::common::StringUtil;
using easyhttpcpp::testutil::TestFileUtil;

namespace easyhttpcpp {
namespace test {

static const std::string Tag = "HttpFileCacheIntegrationTest";
static const char* const TestDataForCacheFromDb = "/HttpIntegrationTest/01_cache_from_db/HttpCache/cache";
static const char* const LruQuery1 = "test=1";
static const char* const LruQuery3 = "test=3";
static const char* const LruQuery4 = "test=4";
static const size_t ResponseBufferBytes = 8192;
static const char* const CacheResponseBodyFileExtention = ".data";

static const char* Test1Url = "http://localhost:9982/test1?a=10";
static const Request::HttpMethod Test1HttpMethod = Request::HttpMethodGet;
static const int Test1StatusCode = Poco::Net::HTTPResponse::HTTP_OK;
static const char* const Test1StatusMessage = "OK";
static const char* const Test1Header1 = "x-header1";
static const char* const Test1HeaderValue1 = "x-value1";
static const char* const Test1ResponseBody = "test1 response body";
static const char* const Test1SentRequestTime = "Fri, 05 Aug 2016 12:00:00 GMT";
static const char* const Test1ReceiveResponseTime = "Fri, 05 Aug 2016 12:00:10 GMT";
static const char* const Test1CreatedMetadataTime = "Fri, 05 Aug 2016 12:00:20 GMT";
static const char* const Test1TempFilename = "tempFile0001";

static const size_t JustCacheMaxSize = 300;
static const size_t CacheOverResponseBodySize = 400;

class HttpFileCacheIntegrationTest : public HttpIntegrationTestCase {
protected:

    void SetUp()
    {
        Poco::Path path(HttpTestUtil::getDefaultCachePath());
        FileUtil::removeDirsIfPresent(path);
    }
};

namespace {

HttpCacheMetadata* createHttpCacheMetadata(const std::string& key, const std::string& url,
        size_t responseBodySize)
{
    HttpCacheMetadata* pHttpCacheMetadata = new HttpCacheMetadata();
    pHttpCacheMetadata->setKey(key);
    pHttpCacheMetadata->setUrl(url);
    pHttpCacheMetadata->setHttpMethod(Test1HttpMethod);
    pHttpCacheMetadata->setStatusCode(Test1StatusCode);
    pHttpCacheMetadata->setStatusMessage(Test1StatusMessage);
    Headers::Ptr pHeaders = new Headers();
    pHeaders->set(Test1Header1, Test1HeaderValue1);
    pHttpCacheMetadata->setResponseHeaders(pHeaders);
    pHttpCacheMetadata->setResponseBodySize(responseBodySize);
    Poco::Timestamp timeStamp;
    HttpUtil::tryParseDate(Test1SentRequestTime, timeStamp);
    pHttpCacheMetadata->setSentRequestAtEpoch(timeStamp.epochTime());
    HttpUtil::tryParseDate(Test1ReceiveResponseTime, timeStamp);
    pHttpCacheMetadata->setReceivedResponseAtEpoch(timeStamp.epochTime());
    HttpUtil::tryParseDate(Test1CreatedMetadataTime, timeStamp);
    pHttpCacheMetadata->setCreatedAtEpoch(timeStamp.epochTime());
    return pHttpCacheMetadata;
}

std::string createResponseTempFile()
{
    Poco::File tempDir(HttpTestUtil::getDefaultCacheTempDir());
    tempDir.createDirectories();
    std::string tempFilename = StringUtil::format("%s%s", HttpTestUtil::getDefaultCacheTempDir().c_str(),
            Test1TempFilename);
    Poco::FileOutputStream tempFileStream(tempFilename, std::ios::out | std::ios::trunc | std::ios::binary);
    tempFileStream.write(Test1ResponseBody, strlen(Test1ResponseBody));
    tempFileStream.close();
    return tempFilename;
}

std::string createResponseTempFileBySize(size_t responseBodySize)
{
    Poco::File tempDir(HttpTestUtil::getDefaultCacheTempDir());
    tempDir.createDirectories();
    std::string tempFilename = HttpTestUtil::getDefaultCacheTempDir() + Test1TempFilename;
    Poco::FileOutputStream tempFileStream(tempFilename, std::ios::out | std::ios::trunc | std::ios::binary);
    Poco::Buffer<char> buffer(responseBodySize);
    for (size_t i = 0; i < responseBodySize; i++) {
        buffer[i] = (i % 26) + 'a';
    }
    tempFileStream.write(buffer.begin(), responseBodySize);
    tempFileStream.close();
    return tempFilename;
}

} /* namespace */

// getMetadata
// Cache にないkeyでのgetMetadata
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsFalse_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestConstants::DefaultTestUrlWithQuery;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getMetadata by not exist key 
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    EXPECT_FALSE(httpFileCache.getMetadata(key, pCacheMetadata));
}

// getMetadata
// Cache にある key でのgetMetadata １回目の呼び出し (初回のDatabase からのよみこみ)
//
// true が返る。
// CacheMetadata は、Databaseに格納されている情報が格納される。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database (exist Lru1Query1);
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getMetadata by exist key 
    // Then: return true and get metadata
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    EXPECT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // check response to database
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());
}

// getMetadata
// reservedRemove のkey でgetMetadata
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsFalse_WhenReservedRemoveUrl)
{
    // Given: load test database  and set reservedRemove flag
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getData (dataRefCount++)
    std::istream* pStream;
    ASSERT_TRUE(httpFileCache.getData(key, pStream));
    Poco::SharedPtr<std::istream> pStreamPtr = pStream;

    // set reservedRemove flag.
    ASSERT_TRUE(httpFileCache.remove(key));

    // When: getMetadata
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    EXPECT_FALSE(httpFileCache.getMetadata(key, pCacheMetadata));
}

// getMetadata
// DatabaseのgetMetadataが失敗
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsFalse_WhenErrorOccurredInDatabaseAccess)
{
    // Given: load test database and delete datebase file.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getMetadata for load Database
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    ASSERT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // delete Metadata from database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    ASSERT_TRUE(db.deleteMetadata(key));

    // When: getMetadata
    // Then: return false
    EXPECT_FALSE(httpFileCache.getMetadata(key, pCacheMetadata));
}

// getMetadata
// updateLastAccessSec が失敗
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsFalse_WhenUpdateLastAccessSecFailed)
{
    // Given: load database and set read only permission to database file
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getMetadata for load Database
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    ASSERT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // set read only to database in order to fail updateLastAccessSec.
    Poco::File databaseFile(HttpTestUtil::getDefaultCacheDatabaseFile());
    databaseFile.setReadOnly(true);

    // When: getMetadata
    // Then: return false
    EXPECT_FALSE(httpFileCache.getMetadata(key, pCacheMetadata));
}

// getMetadata
// getMetadata で、LRU の順番がかわる。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ChangesLruList)
{
    // Given: load test database and maxCacheSize is just loaded database total size
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, JustCacheMaxSize);

    // When: getMetadata (LRU_QUERY3)
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    CacheMetadata::Ptr pCacheMetadata3 = new HttpCacheMetadata();
    EXPECT_TRUE(httpFileCache.getMetadata(key3, pCacheMetadata3));

    // Then: change LRU List
    // put new metadata and oldest metadata (LRU_QUERY1) is deleted.
    std::string url0 = Test1Url;
    std::string key0 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url0);
    CacheMetadata::Ptr pCacheMetadata0 = createHttpCacheMetadata(key0, url0, strlen(Test1ResponseBody));
    std::string tempFilePath0 = createResponseTempFile();
    EXPECT_TRUE(httpFileCache.put(key0, pCacheMetadata0, tempFilePath0));

    // oldest cache is deleted
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_FALSE(db.getMetadataAll(key1, metadata));
    EXPECT_TRUE(db.getMetadataAll(key3, metadata));
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_TRUE(db.getMetadataAll(key4, metadata));
}

// getMetadata
// purge の後のgetMetadata
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getMetadata_ReturnsFalse_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getMetadata by not exist key 
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    EXPECT_FALSE(httpFileCache.getMetadata(key, pCacheMetadata));
}

// getData
// Cache にないkeyでのgetData
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getData_ReturnsFalse_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestConstants::DefaultTestUrlWithQuery;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getMetadata by not exist key 
    // Then: return false
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.getData(key, pStream));
}

// getData
// Cache にある key でのgetData １回目の呼び出し (初回のDatabase からのよみこみ)
//
// true が返る。
// response body がstream から取得できる。
TEST_F(HttpFileCacheIntegrationTest, getData_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database (exist LRU_QUERY1))
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getData by exist key 
    // Then: return true and get stream.
    std::istream* pStream = NULL;
    EXPECT_TRUE(httpFileCache.getData(key, pStream));
    Poco::SharedPtr<std::istream> pStreamPtr = pStream;

    // check data
    Poco::Buffer<char> responseBodyBuffer(ResponseBufferBytes);
    pStream->read(responseBodyBuffer.begin(), ResponseBufferBytes);
    size_t readBytes = pStream->gcount();

    // check cached file.
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url));
    ASSERT_EQ(readBytes, responseBodyFile.getSize());
    Poco::FileInputStream responseBodyStream(responseBodyFile.path(), std::ios::in | std::ios::binary);
    Poco::Buffer<char> inBuffer(readBytes);
    responseBodyStream.read(inBuffer.begin(), readBytes);
    ASSERT_EQ(readBytes, responseBodyStream.gcount());
    ASSERT_EQ(0, memcmp(inBuffer.begin(), responseBodyBuffer.begin(), readBytes));
}

// getData
// reservedRemove のkey でgetData
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getData_ReturnsFalse_WhenReservedRemoveUrl)
{
    // Given: load test database and set reservedRemove flag
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getData (dataRefCount++)
    std::istream* pStream;
    ASSERT_TRUE(httpFileCache.getData(key, pStream));
    Poco::SharedPtr<std::istream> pStreamPtr = pStream;

    // set reservedRemove flag.
    ASSERT_TRUE(httpFileCache.remove(key));

    // When: getData
    // Then: return false
    std::istream* pStream2 = NULL;
    EXPECT_FALSE(httpFileCache.getData(key, pStream2));
}

// getData
// response body データがない
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getData_ReturnsFalse_WhenNotExistResponseBodyFile)
{
    // Given: load test database and delete chached file.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    Poco::Path cachedFilePath(HttpTestUtil::getDefaultCacheRootDir(), key + CacheResponseBodyFileExtention);
    Poco::File cachedFile(cachedFilePath);
    cachedFile.remove(false);

    // When: getData by exist key 
    // Then: return false
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.getData(key, pStream));
}
// getData
// purge の後のgetData
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, getData_ReturnsFalse_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: getMetadata by not exist key 
    // Then: return false
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.getData(key, pStream));
}

// get
// Cache にないkeyでのget
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestConstants::DefaultTestUrlWithQuery;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: get by not exist key 
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata, pStream));
}

// get
// Cache にある key でのget １回目の呼び出し (初回のDatabase からのよみこみ)
//
// true が返る。
// CacheMetadata は、Databaseに格納されている情報が格納される。
// response body がstream から取得できる。

TEST_F(HttpFileCacheIntegrationTest, get_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: get by exist key 
    // Then: return true get metadta and stream
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    std::istream* pStream = NULL;
    EXPECT_TRUE(httpFileCache.get(key, pCacheMetadata, pStream));
    Poco::SharedPtr<std::istream> pStreamPtr = pStream;

    // check response to database
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());

    // check data
    Poco::Buffer<char> responseBodyBuffer(ResponseBufferBytes);
    pStream->read(responseBodyBuffer.begin(), ResponseBufferBytes);
    size_t readBytes = pStream->gcount();

    // check cached file.
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url));
    ASSERT_EQ(readBytes, responseBodyFile.getSize());
    Poco::FileInputStream responseBodyStream(responseBodyFile.path(), std::ios::in | std::ios::binary);
    Poco::Buffer<char> inBuffer(readBytes);
    responseBodyStream.read(inBuffer.begin(), readBytes);
    ASSERT_EQ(readBytes, responseBodyStream.gcount());
    ASSERT_EQ(0, memcmp(inBuffer.begin(), responseBodyBuffer.begin(), readBytes));
}

// get
// reservedRemove のkey でget
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenReservedRemoveUrl)
{
    // Given: load test database and set reservedRemove flag
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getData (dataRefCount++)
    std::istream* pStream = NULL;
    ASSERT_TRUE(httpFileCache.getData(key, pStream));
    Poco::SharedPtr<std::istream> pStreamPtr = pStream;

    // set reservedRemove flag.
    ASSERT_TRUE(httpFileCache.remove(key));

    // When: get
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata2 = new HttpCacheMetadata;
    std::istream* pStream2 = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata2, pStream2));
}

// get
// DatabaseのgetMetadataが失敗
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenErrorOccurredInDatabaseAccess)
{
    // Given: load test database and delete metadata.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getMetadata for load Database
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    ASSERT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // delete Metadata from database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    ASSERT_TRUE(db.deleteMetadata(key));

    // When: get
    // Then: return false
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata, pStream));
}

// get
// updateLastAccessSec が失敗
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenUpdateLastAccessSecFailed)
{
    // Given: load test database and set read only to database file.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getMetadata for load Database
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    ASSERT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // set read only to database in order to fail updateLastAccessSec.
    Poco::File databaseFile(HttpTestUtil::getDefaultCacheDatabaseFile());
    databaseFile.setReadOnly(true);

    // When: get
    // Then: return false
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata, pStream));
}

// get
// response body データがない
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenNotExistResponseBodyFile)
{
    // Given: load test database and delete cache file.
    // prepare test data
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    Poco::Path cachedFilePath(HttpTestUtil::getDefaultCacheRootDir(), key + CacheResponseBodyFileExtention);
    Poco::File cachedFile(cachedFilePath);
    cachedFile.remove(false);

    // When: get
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata, pStream));
}

// get
// purge の後のget
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, get_ReturnsFalse_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: get by not exist key 
    // Then: return false
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    std::istream* pStream = NULL;
    EXPECT_FALSE(httpFileCache.get(key, pCacheMetadata, pStream));
}

// putMetadata
// Cache にないkeyでのputMetadata
//
// 1. false が返る。
TEST_F(HttpFileCacheIntegrationTest, putMetadata_ReturnsTrue_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));

    // When: putMetadata by not exist key 
    // Then: return false
    EXPECT_FALSE(httpFileCache.putMetadata(key, pCacheMetadata));
}

// putMetadata
// 存在する key での putMetadata
//
// 1. true が返る。
// 2. Cache が上書きされる。
TEST_F(HttpFileCacheIntegrationTest, putMetadata_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database.
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));

    // When: putMetadata by exist key 
    // Then: return true
    EXPECT_TRUE(httpFileCache.putMetadata(key, pCacheMetadata));

    // check cache
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());
}

// putMetadata
// reservedRemove の key へのputMetadata
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, putMetadata_ReturnsFalse_WhenPutToUrlOfReservedRemove)
{
    // Given: load test database and set reservedRemove flag
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // set reservedRemove flag
    ASSERT_TRUE(httpFileCache.remove(key1));

    std::string url = Test1Url;
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key1, url, strlen(Test1ResponseBody));

    // When: putMetadata
    // Then: return false
    EXPECT_FALSE(httpFileCache.putMetadata(key1, pCacheMetadata));
}

// putMetadata
// dateRefCont > 0 の key への putMetadata
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, putMetadata_ReturnsFalse_WhenPutToDataRefCountIsNotZero)
{
    // Given: load test database and increment dataRefCount
    // prepare test data
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    std::string url = Test1Url;
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key1, url, strlen(Test1ResponseBody));

    // When: putMetadata
    // Then: return false
    EXPECT_FALSE(httpFileCache.putMetadata(key1, pCacheMetadata));
}

// putMetadata
// purge の後のputMetadata
//
// 1. false が返る。
TEST_F(HttpFileCacheIntegrationTest, putMetadata_ReturnsTrue_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));

    // When: putMetadata by not exist key 
    // Then: return false
    EXPECT_FALSE(httpFileCache.putMetadata(key, pCacheMetadata));
}

// put
// Cache にないkeyでのput
//
// 1. true が返る。
// 2. Cache が新規に作成される。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsTrue_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put by not exist key 
    // Then: return true and put metadata and file.
    EXPECT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // check cache
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(HttpTestUtil::getDefaultCachePath()));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());

    // check data
    size_t expectResponseBodySize = strlen(Test1ResponseBody);
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(HttpTestUtil::getDefaultCachePath(),
            Request::HttpMethodGet, url));
    ASSERT_EQ(expectResponseBodySize, responseBodyFile.getSize());
    Poco::FileInputStream responseBodyStream(responseBodyFile.path(), std::ios::in | std::ios::binary);
    Poco::Buffer<char> inBuffer(expectResponseBodySize);
    responseBodyStream.read(inBuffer.begin(), expectResponseBodySize);
    ASSERT_EQ(expectResponseBodySize, responseBodyStream.gcount());
    ASSERT_EQ(0, memcmp(inBuffer.begin(), Test1ResponseBody, expectResponseBodySize));
}

// put
// 存在する key での put
//
// 1. true が返る。
// 2. Cache が上書きされる。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put by not exist key 
    // Then: return true and put metadata and file.
    EXPECT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // check cache
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());

    // check data
    size_t expectResponseBodySize = strlen(Test1ResponseBody);
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url));
    ASSERT_EQ(expectResponseBodySize, responseBodyFile.getSize());
    Poco::FileInputStream responseBodyStream(responseBodyFile.path(), std::ios::in | std::ios::binary);
    Poco::Buffer<char> inBuffer(expectResponseBodySize);
    responseBodyStream.read(inBuffer.begin(), expectResponseBodySize);
    ASSERT_EQ(expectResponseBodySize, responseBodyStream.gcount());
    ASSERT_EQ(0, memcmp(inBuffer.begin(), Test1ResponseBody, expectResponseBodySize));
}

// put
// put すると、CacheMaxSize を超える場合
//
// 1. LRUで古いcache が削除されて、put される。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsTrueAndOldestCacheIsDeleted_WhenCacheMaxSizeOver)
{
    // Given: load test database and maxCacheSize is just database total size.
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, JustCacheMaxSize);

    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put
    // Then: return true
    EXPECT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // oldest cache(LRU_QUERY3) is deleted
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata));
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_TRUE(db.getMetadataAll(key1, metadata));
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_TRUE(db.getMetadataAll(key4, metadata));
}

// put
// put すると、CacheMaxSize を超えるが、LRUで削除しても、空き容量が足りない場合
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsFalse_WhenNoCapacityToWriteResponseBody)
{
    // Given: load test database and maxCacheSize is just database total size.
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, JustCacheMaxSize);

    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, CacheOverResponseBodySize);
    std::string tempFilePath = createResponseTempFileBySize(CacheOverResponseBodySize);

    // When: put large data
    // Then: return false
    EXPECT_FALSE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // cache does not changed.
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_TRUE(db.getMetadataAll(key3, metadata));
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_TRUE(db.getMetadataAll(key1, metadata));
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_TRUE(db.getMetadataAll(key4, metadata));
}

// put
// put すると、CacheMaxSize を超えるが、dataRefCount > 0 のため、削除しても、空き容量が足りない場合
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsFalse_WhenNoCapacityToWriteResponseBodyByDataRefCountIsNotZero)
{
    // Given: load test database and maxCacheSize is just database total size and increment dataRefCount
    // test data LRU (Query) old -> new
    // LRU_QUERY3 (response body == 100 Byes)
    // LRU_QUERY1 (response body == 100 Byes)
    // LRU_QUERY4 (response body == 100 Byes)

    // LRU_QUERY3, LRU_QUERY1 is dataRefCount > 0
    // CacheMaxSize == 300
    // put Cache data is 150 Bytes

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, JustCacheMaxSize);

    // increment dataRefCount (LRU_QUERY3, LRU_QUERY4))
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    std::istream* pStream3 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key3, pStream3));
    Poco::SharedPtr<std::istream> pStreamPtr3 = pStream3;

    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    std::istream* pStream4 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key4, pStream4));
    Poco::SharedPtr<std::istream> pStreamPtr4 = pStream4;

    size_t responseBodySize = 150;
    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, responseBodySize);
    std::string tempFilePath = createResponseTempFileBySize(responseBodySize);

    // When: put
    // Then: return false
    EXPECT_FALSE(httpFileCache.put(key, pCacheMetadata, tempFilePath));
}

// put
// reservedRemove の key へのput
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsFalse_WhenPutToUrlOfReservedRemove)
{
    // Given: oad test database and set reservedRemove flag
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // set reservedRemove flag.
    ASSERT_TRUE(httpFileCache.remove(key1));

    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key1, url1, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put
    // Then: return false
    EXPECT_FALSE(httpFileCache.put(key1, pCacheMetadata, tempFilePath));
}

// put
// dateRefCont > 0 の key への put
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsFalse_WhenPutToDataRefCountIsNotZero)
{
    // Given: load test database and increment dataRefCount
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCont++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key1, url1, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put
    // Then: return false
    EXPECT_FALSE(httpFileCache.put(key1, pCacheMetadata, tempFilePath));
}

// put
// temp file のmove でエラーとなる場合
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsFalse_WhenErrorOccurredInMoveTempFile)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));

    // tempFile not exist.
    std::string tempFilePath = HttpTestUtil::getDefaultCacheTempDir() + Test1TempFilename;

    // When: put
    // Then: return false
    EXPECT_FALSE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // do not create cache
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(HttpTestUtil::getDefaultCachePath()));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_FALSE(db.getMetadataAll(key, metadata));
}

// put
// purge の後の put
//
// 1. true が返る。
// 2. Cache が新規に作成される。
TEST_F(HttpFileCacheIntegrationTest, put_ReturnsTrue_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();

    // When: put by not exist key 
    // Then: return true
    EXPECT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // check cache
    HttpCacheMetadata* pHttpCacheMetadata = static_cast<HttpCacheMetadata*> (pCacheMetadata.get());
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(HttpTestUtil::getDefaultCachePath()));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
    EXPECT_EQ(metadata.getKey(), pHttpCacheMetadata->getKey());
    EXPECT_EQ(metadata.getUrl(), pHttpCacheMetadata->getUrl());
    EXPECT_EQ(metadata.getHttpMethod(), pHttpCacheMetadata->getHttpMethod());
    EXPECT_EQ(metadata.getStatusCode(), pHttpCacheMetadata->getStatusCode());
    EXPECT_EQ(metadata.getStatusMessage(), pHttpCacheMetadata->getStatusMessage());
    EXPECT_THAT(pHttpCacheMetadata->getResponseHeaders(), testutil::equalHeaders(metadata.getResponseHeaders()));
    EXPECT_EQ(metadata.getResponseBodySize(), pHttpCacheMetadata->getResponseBodySize());
    EXPECT_EQ(metadata.getSentRequestAtEpoch(), pHttpCacheMetadata->getSentRequestAtEpoch());
    EXPECT_EQ(metadata.getReceivedResponseAtEpoch(), pHttpCacheMetadata->getReceivedResponseAtEpoch());
    EXPECT_EQ(metadata.getCreatedAtEpoch(), pHttpCacheMetadata->getCreatedAtEpoch());

    // check data
    size_t expectResponseBodySize = strlen(Test1ResponseBody);
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url));
    ASSERT_EQ(expectResponseBodySize, responseBodyFile.getSize());
    Poco::FileInputStream responseBodyStream(responseBodyFile.path(), std::ios::in | std::ios::binary);
    Poco::Buffer<char> inBuffer(expectResponseBodySize);
    responseBodyStream.read(inBuffer.begin(), expectResponseBodySize);
    ASSERT_EQ(expectResponseBodySize, responseBodyStream.gcount());
    ASSERT_EQ(0, memcmp(inBuffer.begin(), Test1ResponseBody, expectResponseBodySize));
}

// remove
// Cache にないkeyでのremove
//
// 1. false が返る。
TEST_F(HttpFileCacheIntegrationTest, remove_ReturnsFalse_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: remove by not exist key 
    // Then: return false
    EXPECT_FALSE(httpFileCache.remove(key));
}

// remove
// 存在する key での remove
//
// 1. true が返る。
// 2. cache から削除される。
TEST_F(HttpFileCacheIntegrationTest, remove_ReturnsTrue_WhenExistKeyInCache)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: remove
    // Then: return true
    EXPECT_TRUE(httpFileCache.remove(key));

    // delete from database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_FALSE(db.getMetadataAll(key, metadata));

    // response body in cache was deleted.
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url));
    EXPECT_FALSE(responseBodyFile.exists());
}

// remove
// dateRefCont > 0 の key への remove
//
// 1. true が返る。
// 2. まだCache から削除されない。
TEST_F(HttpFileCacheIntegrationTest, remove_ReturnsTrue_WhenRemoveToDataRefCountIsNotZero)
{
    // Given: load test database and increment dataRefCount
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // increment dataRefCount
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // When: remove
    // Then: return true
    EXPECT_TRUE(httpFileCache.remove(key1));

    // yet delete from cache
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_TRUE(db.getMetadataAll(key1, metadata1));

    // response body in cache was not deleted.
    Poco::File responseBodyFile(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath,
            Request::HttpMethodGet, url1));
    EXPECT_TRUE(responseBodyFile.exists());
}

// remove
// purge の後の remove
//
// false が返る。
TEST_F(HttpFileCacheIntegrationTest, remove_ReturnsFalse_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: remove by not exist key 
    // Then: return false
    EXPECT_FALSE(httpFileCache.remove(key));
}

// releaseData
// 存在しない key での releaseData
//
// 何も起こらない。
TEST_F(HttpFileCacheIntegrationTest, releaseData_NothingHappens_WhenNotExistKeyInCache)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestConstants::DefaultTestUrlWithQuery;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: releaseData by not exist key 
    // Then: nothing happens.
    httpFileCache.releaseData(key);

    // check database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(HttpTestUtil::getDefaultCachePath()));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_FALSE(db.getMetadataAll(key, metadata));
}

// releaseData
// dataRefCount == 0 の key での releaseData
//
// 何も起こらない。
TEST_F(HttpFileCacheIntegrationTest, releaseData_NothingHappens_WhenDataRefCountEqualsZero)
{
    // Given: load test database
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: releaseData by exist key 
    // Then: nothing happens.
    httpFileCache.releaseData(key);

    // check database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_TRUE(db.getMetadataAll(key, metadata));
}

// releaseData
// dataRefCount == 1 の key での releaseData
//
// 1. dataRefCount が 0 となる。
//    この後、remove してCache から削除されることで確認。
TEST_F(HttpFileCacheIntegrationTest, releaseData_SetsDataRefCountToZero_WhenDataRefCountIsOne)
{
    // Given: load test database and increment dataRefCount
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // When: releaseData
    // Then: set dataRefCount to 0
    httpFileCache.releaseData(key1);

    // confirm to remove
    pStreamPtr1 = NULL;
    EXPECT_TRUE(httpFileCache.remove(key1));

    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
}

// releaseData
// dataRefCount == 2 の key での releaseData を 2回呼び出す
//
// 1. 1回目のreleaseDataではdataRefCount が 1 となる。
//    この後、remove しもCache からすぐ削除されれないことで確認。
// 2. 2回目のreleaseDataでdataRefCount が 0 となる。
//    この後、remove してCache から削除されることで確認。
TEST_F(HttpFileCacheIntegrationTest,
        releaseData_SetsDataRefCountToZero_WhenDataRefCountIsTwoAndCallReleaseDataTwoTimes)
{
    // Given: laod test database and dataRefCount = 2
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (daaRefCont = 1)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // dataRefCount = 2
    std::istream* pStream2 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream2));
    Poco::SharedPtr<std::istream> pStreamPtr2 = pStream2;

    pStreamPtr1 = NULL;
    pStreamPtr2 = NULL;

    // releaseData
    // set dataRefCount to 1
    httpFileCache.releaseData(key1);

    // dataReCount == 1
    EXPECT_TRUE(httpFileCache.remove(key1));

    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_TRUE(db.getMetadataAll(key1, metadata1));

    // When: releaseData
    // Then : dataReCount == 0 and remove from cache
    httpFileCache.releaseData(key1);
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
}

// releaseData
// dataRefCount == 1 で、reservedRemove == true の key での releaseData
//
// Cache から削除される。
TEST_F(HttpFileCacheIntegrationTest,
        releaseData_SetsDataRefCountToZeroAndRemovesFromCache_WhenDataRefCountIsOneAndSetReservedRemove)
{
    // Given: load test database increment dataRefCount and set reservedRemove flag
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;
    // set reservedRemove flag
    ASSERT_TRUE(httpFileCache.remove(key1));

    // When: releaseData
    // Then: set dataRefCount to 0 and remove from cache
    httpFileCache.releaseData(key1);

    // confirm to remove
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
}

// releaseData
// purge の後の releaseData
//
// 何も起こらない。
TEST_F(HttpFileCacheIntegrationTest, releaseData_NothingHappens_WhenAfterPurge)
{
    // Given: purge cache

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);
    
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    ASSERT_TRUE(httpFileCache.purge(true));

    std::string url = HttpTestConstants::DefaultTestUrlWithQuery;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // When: releaseData by not exist key 
    // Then: nothing happens.
    httpFileCache.releaseData(key);

    // check database
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    EXPECT_FALSE(db.getMetadataAll(key, metadata));
}

// purge
// mayDeleteIfBusy == true で、dataRefCount > 0 の key がない場合のpurge
//
// 全てのCache が削除される。
TEST_F(HttpFileCacheIntegrationTest,
        purge_ReturnsTrueAndRemovesAllCache_WhenMayDeleteIfBusyIsTrueAndAllDataRefCountIsZero)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // When: purge
    // Then: all data remove from cache
    EXPECT_TRUE(httpFileCache.purge(true));

    // confirm to remove
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
    HttpCacheDatabase::HttpCacheMetadataAll metadata3;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata3));
    HttpCacheDatabase::HttpCacheMetadataAll metadata4;
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_FALSE(db.getMetadataAll(key4, metadata1));

    // response body in cache was deleted.
    Poco::File responseBodyFile1(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url1));
    EXPECT_FALSE(responseBodyFile1.exists());
    Poco::File responseBodyFile3(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url3));
    EXPECT_FALSE(responseBodyFile3.exists());
    Poco::File responseBodyFile4(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url4));
    EXPECT_FALSE(responseBodyFile4.exists());
}

// purge
// mayDeleteIfBusy == true で、dataRefCount > 0 の key がある場合のpurge
//
// 全てのCache が削除される。
TEST_F(HttpFileCacheIntegrationTest,
        purge_ReturnsTrueAndRemovesAllCache_WhenMayDeleteIfBusyIsTrueAndExistsDataRefCountGreaterThanZero)
{
    // Given: load test database and increment dataRefCont
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dateRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // When: purge (mayDeleteIfBusy == true)
    // Then: all data remove from cache
    EXPECT_TRUE(httpFileCache.purge(true));

    // confirm to remove
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
    HttpCacheDatabase::HttpCacheMetadataAll metadata3;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata3));
    HttpCacheDatabase::HttpCacheMetadataAll metadata4;
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_FALSE(db.getMetadataAll(key4, metadata1));

    // response body in cache was deleted.
    Poco::File responseBodyFile1(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url1));
    EXPECT_FALSE(responseBodyFile1.exists());
    Poco::File responseBodyFile3(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url3));
    EXPECT_FALSE(responseBodyFile3.exists());
    Poco::File responseBodyFile4(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url4));
    EXPECT_FALSE(responseBodyFile4.exists());
}

// purge
// mayDeleteIfBusy == false  で、dataRefCount > 0 の key がない場合のpurge
//
// 全てのCache が削除される。
TEST_F(HttpFileCacheIntegrationTest,
        purge_ReturnsTrueAndRemovesAllCache_WhenMayDeleteIfBusyIsFalseAndAllDataRefCountIsZero)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // When: purge
    // Then: all data remove from cache
    EXPECT_TRUE(httpFileCache.purge(false));

    // confirm to remove
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_FALSE(db.getMetadataAll(key1, metadata1));
   HttpCacheDatabase::HttpCacheMetadataAll metadata3;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata3));
    HttpCacheDatabase::HttpCacheMetadataAll metadata4;
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_FALSE(db.getMetadataAll(key4, metadata1));

    // response body in cache was deleted.
    Poco::File responseBodyFile1(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url1));
    EXPECT_FALSE(responseBodyFile1.exists());
    Poco::File responseBodyFile3(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url3));
    EXPECT_FALSE(responseBodyFile3.exists());
    Poco::File responseBodyFile4(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url4));
    EXPECT_FALSE(responseBodyFile4.exists());
}

// purge
// mayDeleteIfBusy == false で、dataRefCount > 0 の key がある場合のpurge
//
// 1. false が返る
// 2. dataCount > 0 のCache だけがのこり、その他は削除される。
TEST_F(HttpFileCacheIntegrationTest,
        purge_ReturnsFalseAndRemainOnlyDataRefCountGreaterThenZero_WhenMaDeleteIfBusyIsFalseAndExistsDataRefCountGreaterThanZero)
{
    // Given: load test database and increment dataRefCont
    // test data LRU (Query) old -> new
    // LRU_QUERY3
    // LRU_QUERY1
    // LRU_QUERY4

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // getData (dataRefCount++)
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    std::istream* pStream1 = NULL;
    ASSERT_TRUE(httpFileCache.getData(key1, pStream1));
    Poco::SharedPtr<std::istream> pStreamPtr1 = pStream1;

    // When: purge
    // Then: all data remove from cache
    EXPECT_FALSE(httpFileCache.purge(false));

    // confirm to remove (LRU_QUERY1 is not removed)
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata1;
    EXPECT_TRUE(db.getMetadataAll(key1, metadata1));
    HttpCacheDatabase::HttpCacheMetadataAll metadata3;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata3));
    HttpCacheDatabase::HttpCacheMetadataAll metadata4;
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_FALSE(db.getMetadataAll(key4, metadata1));

    // response body in cache. LRU_QUERY1 is not deleted.
    Poco::File responseBodyFile1(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url1));
    EXPECT_TRUE(responseBodyFile1.exists());
    Poco::File responseBodyFile3(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url3));
    EXPECT_FALSE(responseBodyFile3.exists());
    Poco::File responseBodyFile4(HttpTestUtil::createCachedResponsedBodyFilePath(cachePath, Request::HttpMethodGet,
            url4));
    EXPECT_FALSE(responseBodyFile4.exists());
}

// getSize
// Cache が空の場合の getSize
//
// 0 が返る
TEST_F(HttpFileCacheIntegrationTest, getSize_ReturnsZero_WhenCacheIsEmpty)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // When: getSize
    // Then: return 0
    EXPECT_EQ(0, httpFileCache.getSize());
}

// getSize
// Cache がある場合の getSize (database からのロード時)
//
// response body の合計サイズが返る。
TEST_F(HttpFileCacheIntegrationTest, getSize_ReturnsCacheSize_WhenCacheIsLoadedFromDatabase)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3 (response body == 100 Byes)
    // LRU_QUERY1 (response body == 100 Byes)
    // LRU_QUERY4 (response body == 100 Byes)

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // When: getSize
    // Then: return 300
    EXPECT_EQ(300, httpFileCache.getSize());
}

// getSize
// Cache に存在しないurl  の put
//
// getSize が更新される。
TEST_F(HttpFileCacheIntegrationTest, getSize_ReturnsCacheSize_WhenPutByNotExistUrl)
{
    // Given: empty cache
    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // put
    std::string url = Test1Url;
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();
    ASSERT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // When: getSize
    // Then: return total response body size
    EXPECT_EQ(strlen(Test1ResponseBody), httpFileCache.getSize());
}

// getSize
// Cache に存在するurl  の getData
//
// getSize が更新される。
TEST_F(HttpFileCacheIntegrationTest, getSize_ReturnsCacheSize_WhenPutByExistUrl)
{
    // Given: load test database
    // test data LRU (Query) old -> new
    // LRU_QUERY3 (response body == 100 Byes)
    // LRU_QUERY1 (response body == 100 Byes)
    // LRU_QUERY4 (response body == 100 Byes)

    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    // put
    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);
    CacheMetadata::Ptr pCacheMetadata = createHttpCacheMetadata(key, url, strlen(Test1ResponseBody));
    std::string tempFilePath = createResponseTempFile();
    ASSERT_TRUE(httpFileCache.put(key, pCacheMetadata, tempFilePath));

    // When: getSize
    // Then: return updated response body size
    EXPECT_EQ(300 - 100 + strlen(Test1ResponseBody), httpFileCache.getSize());
}

// getSize
// url3 -> url1 -> url4 の順番の Database (cacheSize == 300)で、CacheMaxSize を100にして、Cache展開する。
//
// url3, url1 がCache から削除される。
TEST_F(HttpFileCacheIntegrationTest, getSize_ResurnsCacheSize_WhenCacheDeleteByLoadDatabase)
{
    // Given: load test database with maxCacheSize = 100
    // prepare test data
    // test data LRU (Query) old -> new
    // LRU_QUERY3 (response body == 100 Byes)
    // LRU_QUERY1 (response body == 100 Byes)
    // LRU_QUERY4 (response body == 100 Byes)
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, 100);

    // When: getSize
    // Then: return response body size
    EXPECT_EQ(100, httpFileCache.getSize());

    // oldest cache is deleted
    HttpCacheDatabase db(HttpTestUtil::createDatabasePath(cachePath));
    HttpCacheDatabase::HttpCacheMetadataAll metadata;
    std::string url3 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery3);
    std::string key3 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url3);
    EXPECT_FALSE(db.getMetadataAll(key3, metadata));
    std::string url1 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key1 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url1);
    EXPECT_FALSE(db.getMetadataAll(key1, metadata));
    std::string url4 = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery4);
    std::string key4 = HttpUtil::makeCacheKey(Request::HttpMethodGet, url4);
    EXPECT_TRUE(db.getMetadataAll(key4, metadata));
}

TEST_F(HttpFileCacheIntegrationTest, onRenmove_ReturnsFalse_WhenDeleteMetadataReturnedFalse)
{
    // Given: load test database and set read only to database file.
    std::string cachePath = HttpTestUtil::getDefaultCachePath();
    Poco::File cacheParentPath(HttpTestUtil::getDefaultCacheParentPath());
    cacheParentPath.createDirectories();
    Poco::File srcTestData(std::string(EASYHTTPCPP_STRINGIFY_MACRO(RUNTIME_DATA_ROOT)) + TestDataForCacheFromDb);
    srcTestData.copyTo(cachePath);

    Poco::Path cacheRootDir(HttpTestUtil::getDefaultCacheRootDir());
    HttpFileCache httpFileCache(cacheRootDir, HttpTestConstants::DefaultCacheMaxSize);

    std::string url = HttpTestUtil::makeUrl(HttpTestConstants::Http, HttpTestConstants::DefaultHost,
            HttpTestConstants::DefaultPort, HttpTestConstants::DefaultPath, LruQuery1);
    std::string key = HttpUtil::makeCacheKey(Request::HttpMethodGet, url);

    // getMetadata for load Database
    CacheMetadata::Ptr pCacheMetadata = new HttpCacheMetadata;
    ASSERT_TRUE(httpFileCache.getMetadata(key, pCacheMetadata));

    // Database を read only にする。
    TestFileUtil::changeAccessPermission(HttpTestUtil::getDefaultCacheDatabaseFile(),
            EASYHTTPCPP_FILE_PERMISSION_ALLUSER_READ_ONLY);

    // When: onRemove
    // Then: return false
    EXPECT_FALSE(httpFileCache.onRemove(key));
}

} /* namespace test */
} /* namespace easyhttpcpp */
