#include <gtest/gtest.h>
#include "caduvelox/http/HttpRouter.hpp"

using namespace caduvelox;

class HttpRouterUnitTest : public ::testing::Test {
protected:
    HttpRouter router;
    HttpRequest req;
    HttpResponse res;

    void SetUp() override {
        router = HttpRouter{};
        req = HttpRequest{};
        res = HttpResponse{};
    }

    void setupBasicRequest(const std::string& method, const std::string& path) {
        req.method = method;
        req.path = path;
        req.version = "HTTP/1.1";
    }
};

TEST_F(HttpRouterUnitTest, RoutesBasicGetRequest) {
    router.get("/users", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("User list");
    });

    setupBasicRequest("GET", "/users");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.status_text, "OK");
    EXPECT_EQ(res.body, "User list");
    EXPECT_EQ(res.getHeader("content-length"), "9");
    EXPECT_EQ(res.getHeader("content-type"), "text/plain");
}

TEST_F(HttpRouterUnitTest, RoutesPostRequest) {
    router.post("/users", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(201, "Created");
        response.setBody("User created");
    });

    setupBasicRequest("POST", "/users");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 201);
    EXPECT_EQ(res.status_text, "Created");
    EXPECT_EQ(res.body, "User created");
}

TEST_F(HttpRouterUnitTest, RoutesPutAndDeleteRequests) {
    router.put("/users/123", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("User updated");
    });

    router.del("/users/123", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(204, "No Content");
        response.setBody("");
    });

    // Test PUT
    setupBasicRequest("PUT", "/users/123");
    router.dispatch(req, res);
    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, "User updated");

    // Reset response and test DELETE
    res = HttpResponse{};
    setupBasicRequest("DELETE", "/users/123");
    router.dispatch(req, res);
    EXPECT_EQ(res.status_code, 204);
    EXPECT_EQ(res.body, "");
}

TEST_F(HttpRouterUnitTest, HandlesRegexRoutes) {
    router.get(R"(/users/(\d+))", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("User details");
    });

    setupBasicRequest("GET", "/users/123");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, "User details");
}

TEST_F(HttpRouterUnitTest, DoesNotMatchInvalidRegex) {
    router.get(R"(/users/(\d+))", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("User details");
    });

    // Should not match - letters instead of digits
    setupBasicRequest("GET", "/users/abc");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 404);
    EXPECT_EQ(res.status_text, "Not Found");
    EXPECT_EQ(res.body, "Not Found");
}

TEST_F(HttpRouterUnitTest, HandlesAllMethodRoute) {
    router.all("/health", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("Healthy");
    });

    // Test with different methods
    std::vector<std::string> methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"};
    
    for (const auto& method : methods) {
        res = HttpResponse{}; // Reset response
        setupBasicRequest(method, "/health");
        router.dispatch(req, res);
        
        EXPECT_EQ(res.status_code, 200) << "Failed for method: " << method;
        EXPECT_EQ(res.body, "Healthy") << "Failed for method: " << method;
    }
}

TEST_F(HttpRouterUnitTest, ReturnsNotFoundForUnmatchedRoute) {
    router.get("/users", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("Users");
    });

    setupBasicRequest("GET", "/nonexistent");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 404);
    EXPECT_EQ(res.status_text, "Not Found");
    EXPECT_EQ(res.body, "Not Found");
    EXPECT_EQ(res.getHeader("content-type"), "text/plain");
    EXPECT_EQ(res.getHeader("content-length"), "9");
}

TEST_F(HttpRouterUnitTest, ReturnsNotFoundForWrongMethod) {
    router.get("/users", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setBody("Users");
    });

    // Try POST on a GET-only route
    setupBasicRequest("POST", "/users");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 404);
    EXPECT_EQ(res.status_text, "Not Found");
}

TEST_F(HttpRouterUnitTest, HandlesExceptionInHandler) {
    router.get("/error", [](const HttpRequest& request, HttpResponse& response) {
        throw std::runtime_error("Something went wrong");
    });

    setupBasicRequest("GET", "/error");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 500);
    EXPECT_EQ(res.status_text, "Internal Server Error");
    EXPECT_EQ(res.body, "Internal Server Error");
}

TEST_F(HttpRouterUnitTest, PreservesCustomHeaders) {
    router.get("/custom", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setHeader("custom-header", "custom-value");
        response.setHeader("content-type", "application/json");
        response.setBody(R"({"message": "success"})");
    });

    setupBasicRequest("GET", "/custom");
    router.dispatch(req, res);

    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.getHeader("custom-header"), "custom-value");
    EXPECT_EQ(res.getHeader("content-type"), "application/json");
    EXPECT_EQ(res.body, R"({"message": "success"})");
}

TEST_F(HttpRouterUnitTest, OverridesContentLengthWhenBodyIsSet) {
    router.get("/explicit", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setHeader("content-length", "99");
        response.setBody("short"); // This will override content-length to actual body length
    });

    setupBasicRequest("GET", "/explicit");
    router.dispatch(req, res);

    // setBody should override content-length with actual body length
    EXPECT_EQ(res.getHeader("content-length"), "5"); // Actual length of "short"
    EXPECT_NE(res.getHeader("content-length"), "99"); // Should not keep the explicit value
}

TEST_F(HttpRouterUnitTest, DoesNotOverrideExplicitContentType) {
    router.get("/typed", [](const HttpRequest& request, HttpResponse& response) {
        response.setStatus(200, "OK");
        response.setHeader("content-type", "application/xml");
        response.setBody("<xml/>");
    });

    setupBasicRequest("GET", "/typed");
    router.dispatch(req, res);

    EXPECT_EQ(res.getHeader("content-type"), "application/xml");
}

TEST_F(HttpRouterUnitTest, HandlesMultipleRoutes) {
    router.get("/route1", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("Route 1");
    });
    
    router.get("/route2", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("Route 2");
    });
    
    router.post("/route1", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("POST Route 1");
    });

    // Test first GET route
    setupBasicRequest("GET", "/route1");
    router.dispatch(req, res);
    EXPECT_EQ(res.body, "Route 1");

    // Test second GET route
    res = HttpResponse{};
    setupBasicRequest("GET", "/route2");
    router.dispatch(req, res);
    EXPECT_EQ(res.body, "Route 2");

    // Test POST route
    res = HttpResponse{};
    setupBasicRequest("POST", "/route1");
    router.dispatch(req, res);
    EXPECT_EQ(res.body, "POST Route 1");
}

TEST_F(HttpRouterUnitTest, FirstMatchingRouteWins) {
    // Add two routes that could match the same request
    router.get("/test", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("First handler");
    });
    
    router.get("/test", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("Second handler");
    });

    setupBasicRequest("GET", "/test");
    router.dispatch(req, res);

    EXPECT_EQ(res.body, "First handler");
}

TEST_F(HttpRouterUnitTest, CanCopyRouter) {
    router.get("/original", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("Original route");
    });

    // Copy the router
    HttpRouter router_copy = router;
    
    // Add route to original (should not affect copy)
    router.get("/new", [](const HttpRequest& request, HttpResponse& response) {
        response.setBody("New route");
    });

    // Test that copy still has original route
    setupBasicRequest("GET", "/original");
    router_copy.dispatch(req, res);
    EXPECT_EQ(res.body, "Original route");

    // Test that copy doesn't have new route
    res = HttpResponse{};
    setupBasicRequest("GET", "/new");
    router_copy.dispatch(req, res);
    EXPECT_EQ(res.status_code, 404);
}
