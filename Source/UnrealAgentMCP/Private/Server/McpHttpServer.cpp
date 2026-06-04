#include "Server/McpHttpServer.h"

#include "HttpPath.h"
#include "HttpServerConstants.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Server/McpProtocol.h"
#include "UnrealAgentMCPModule.h"

bool FMcpHttpServer::Start(uint32 Port)
{
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	// StartAllListeners must precede GetHttpRouter: the engine only attempts the actual socket
	// bind once listeners are enabled. Calling GetHttpRouter(bFailOnBindFailure=true) before
	// enabling them defers the bind into StartAllListeners(), which swallows failures — Start()
	// would report success with no socket bound (UE 5.5 HttpServerModule.cpp:54,158-179).
	HttpServerModule.StartAllListeners();

	Router = HttpServerModule.GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogAgentMcp, Error, TEXT("Failed to bind MCP server on port %u (already in use?)"), Port);
		return false;
	}

	RouteHandle = Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMcpHttpServer::HandleRequest));
	if (!RouteHandle.IsValid())
	{
		UE_LOG(LogAgentMcp, Error, TEXT("Failed to bind route POST /mcp"));
		Router.Reset();
		return false;
	}

	UE_LOG(LogAgentMcp, Display, TEXT("MCP server listening on http://127.0.0.1:%u/mcp"), Port);
	return true;
}

void FMcpHttpServer::Stop()
{
	if (Router.IsValid() && RouteHandle.IsValid())
	{
		Router->UnbindRoute(RouteHandle);
	}
	RouteHandle.Reset();
	Router.Reset();
}

bool FMcpHttpServer::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Editor mutation APIs (FScopedTransaction, K2 node ops) require the game thread. The engine
	// HTTPServer ticks listeners on the game thread (verified against 5.5 LaunchEngineLoop/FTSTicker
	// in the P1 review). If a future engine changes dispatch, degrade gracefully instead of killing
	// the editor: refuse the request with a JSON-RPC internal error (building JSON touches no editor state).
	if (!ensureMsgf(IsInGameThread(), TEXT("MCP HandleRequest dispatched off the game thread; refusing request")))
	{
		TUniquePtr<FHttpServerResponse> ErrorResponse = FHttpServerResponse::Create(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Server thread-affinity violation; request refused\"}}"),
			TEXT("application/json"));
		OnComplete(MoveTemp(ErrorResponse));
		return true;
	}

	// TODO(P2): enforce Content-Type: application/json (415 otherwise). P1 is deliberately lenient —
	// non-JSON bodies fall through to the protocol layer's -32700 parse error.
	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString Body(Converter.Length(), Converter.Get());

	const FString ResponseJson = AgentMcp::Protocol::HandleMessage(Body);

	TUniquePtr<FHttpServerResponse> Response;
	if (ResponseJson.IsEmpty())
	{
		// Notification: MCP streamable-HTTP requires 202 Accepted with no body.
		Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
	}
	else
	{
		Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
	}
	OnComplete(MoveTemp(Response));
	return true;
}
