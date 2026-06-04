#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"  // typedef TFunction<...> — cannot be forward-declared
#include "HttpRouteHandle.h"     // typedef TSharedPtr<...> — cannot be forward-declared

struct FHttpServerRequest;
class IHttpRouter;

/** Owns the /mcp HTTP route. Loopback only (engine HTTPServer default bind is localhost). */
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
