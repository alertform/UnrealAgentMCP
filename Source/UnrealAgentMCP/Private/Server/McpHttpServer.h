#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"  // typedef TFunction<...> — cannot be forward-declared
#include "HttpRouteHandle.h"     // typedef TSharedPtr<...> — cannot be forward-declared

struct FHttpServerRequest;
class IHttpRouter;

/**
 * Owns the /mcp HTTP route. Binds via the engine HTTPServer module, whose DefaultBindAddress
 * defaults to localhost — NOT enforced here. An ini override ([HTTPServer.Listeners]) could widen
 * exposure; the P1 smoke checklist netstat-verifies 127.0.0.1. TODO(P3): programmatically refuse
 * non-loopback bind before destructive tools land.
 */
class FMcpHttpServer
{
public:
	~FMcpHttpServer() { Stop(); }

	/** Binds POST /mcp on the given port and starts listening. False on bind failure (port in use). */
	bool Start(uint32 Port);

	/** Unbinds the route. Listener teardown happens with module shutdown. */
	void Stop();

private:
	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle RouteHandle;
};
