#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class ASuperSLMExampleScene;

// The example scene's panel, built in Slate so the scene needs no widget asset: the demonstrator's
// controls (Thinking/Frame Budget, Backend, concurrent queries, k) on the left; on the right the
// self-check's verdicts with their scope (in 1.0 the GPU verdict is always withheld), the token
// digest, the structured result, the voiced reply, and the cost readout with each figure's source.
class SSuperSLMExampleWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSuperSLMExampleWidget)
		: _Scene(nullptr)
	{}
		SLATE_ARGUMENT(ASuperSLMExampleScene*, Scene)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TWeakObjectPtr<ASuperSLMExampleScene> Scene;
	FString LastAskError;

	bool IsIdle() const;
	FText StatusText() const;
	FText SelfCheckText() const;
	FText ResultText() const;
	FText DigestText() const;
	FText FiguresText() const;
	FText FrameBudgetHint() const;
	FText KHint() const;
	FReply OnAsk();
	FReply OnSelfCheck();
};
