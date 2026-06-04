#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Server/McpProtocol.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace AgentMcpTestUtils
{
	inline TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	/** Creates a transient Actor blueprint that never touches disk (GC'd after the test run). */
	inline UBlueprint* MakeTransientBlueprint(const FString& BaseName)
	{
		const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(*BaseName));
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), UniqueName,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	/** Creates a transient WidgetBlueprint (UUserWidget parent) that never touches disk. */
	inline UWidgetBlueprint* MakeTransientWidgetBlueprint(const TCHAR* BaseName)
	{
		const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UWidgetBlueprint::StaticClass(), FName(BaseName));
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), GetTransientPackage(), UniqueName,
			BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		// Ensure WidgetTree exists (CreateBlueprint may leave it null for transient packages).
		if (WBP && !WBP->WidgetTree)
		{
			WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transactional);
		}
		return WBP;
	}

	/** Calls one MCP tool through the full protocol path; returns the parsed JSON payload of content[0].text. */
	inline TSharedPtr<FJsonObject> CallTool(FAutomationTestBase& Test, const FString& ToolName, const FString& ArgsJson, bool& bOutIsError)
	{
		const FString Request = FString::Printf(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}"),
			*ToolName, *ArgsJson);
		const TSharedPtr<FJsonObject> Response = Parse(AgentMcp::Protocol::HandleMessage(Request));
		bOutIsError = true;
		if (!Test.TestTrue(FString::Printf(TEXT("%s response has result"), *ToolName),
			Response.IsValid() && Response->HasField(TEXT("result"))))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Result = Response->GetObjectField(TEXT("result"));
		bOutIsError = Result->GetBoolField(TEXT("isError"));
		const TArray<TSharedPtr<FJsonValue>>& Content = Result->GetArrayField(TEXT("content"));
		if (Content.Num() == 0)
		{
			return nullptr;
		}
		const FString Text = Content[0]->AsObject()->GetStringField(TEXT("text"));
		return Parse(Text); // nullptr is fine for plain-text error payloads
	}

	/** Raw text of content[0] (for error-message assertions). Empty when no result/content. */
	inline FString CallToolRawText(FAutomationTestBase& Test, const FString& ToolName, const FString& ArgsJson, bool& bOutIsError)
	{
		const FString Request = FString::Printf(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}"),
			*ToolName, *ArgsJson);
		const TSharedPtr<FJsonObject> Response = Parse(AgentMcp::Protocol::HandleMessage(Request));
		bOutIsError = true;
		if (!Response.IsValid() || !Response->HasField(TEXT("result")))
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> Result = Response->GetObjectField(TEXT("result"));
		bOutIsError = Result->GetBoolField(TEXT("isError"));
		const TArray<TSharedPtr<FJsonValue>>& Content = Result->GetArrayField(TEXT("content"));
		return Content.Num() > 0 ? Content[0]->AsObject()->GetStringField(TEXT("text")) : FString();
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
