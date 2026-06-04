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

	HttpServerModule.StartAllListeners();
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
