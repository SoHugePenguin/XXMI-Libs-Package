#pragma once

#include <DirectXMath.h>
#include <d3d11_1.h>
#include <vector>

#include "util.h"
#include "Input.h"
#include "PenguinDV.h"

enum class KeyOverrideType {
	INVALID = -1,
	ACTIVATE,
	HOLD,
	TOGGLE,
	CYCLE,
};
static EnumName_t<const wchar_t *, KeyOverrideType> KeyOverrideTypeNames[] = {
	{L"activate", KeyOverrideType::ACTIVATE},
	{L"hold", KeyOverrideType::HOLD},
	{L"toggle", KeyOverrideType::TOGGLE},
	{L"cycle", KeyOverrideType::CYCLE},
	{NULL, KeyOverrideType::INVALID} // End of list marker
};

enum class TransitionType {
	INVALID = -1,
	LINEAR,
	COSINE,
};
static EnumName_t<const char *, TransitionType> TransitionTypeNames[] = {
	{"linear", TransitionType::LINEAR},
	{"cosine", TransitionType::COSINE},
	{NULL, TransitionType::INVALID} // End of list marker
};

struct OverrideParam
{
	int idx;
	float DirectX::XMFLOAT4::*component;

	OverrideParam(int idx, float DirectX::XMFLOAT4::*component)
	{
		this->idx = idx;
		this->component = component;
	}

	char chr() const
	{
		// Oh come on C++, a pointer to member is just an offset you
		// could test directly... Fine, let's dance:
		switch ((uintptr_t)&((DirectX::XMFLOAT4*)(NULL)->*component)) {
			case (offsetof(DirectX::XMFLOAT4, x)): return 'x';
			case (offsetof(DirectX::XMFLOAT4, y)): return 'y';
			case (offsetof(DirectX::XMFLOAT4, z)): return 'z';
			case (offsetof(DirectX::XMFLOAT4, w)): return 'w';
		}
		return '?';
	};
};
static inline bool operator<(const OverrideParam &lhs, const OverrideParam &rhs)
{
	if (lhs.idx != rhs.idx)
		return (lhs.idx < rhs.idx);
	return ((uintptr_t)&((DirectX::XMFLOAT4*)(NULL)->*(lhs.component)) <
	        (uintptr_t)&((DirectX::XMFLOAT4*)(NULL)->*(rhs.component)));
}
typedef std::map<OverrideParam, float> OverrideParams;
typedef std::map<CommandListVariable*, float> OverrideVars;

class OverrideBase
{
public:
	virtual void ParseIniSection(LPCWSTR section) = 0;
};

class Override : public virtual OverrideBase
{
private:
	int transition, release_transition;
	TransitionType transition_type, release_transition_type;

	bool is_conditional;
	CommandListExpression condition;

	CommandList activate_command_list;
	CommandList deactivate_command_list;

protected:
	bool active;

public:
	OverrideParams mOverrideParams;
	OverrideVars mOverrideVars;

	OverrideParams mSavedParams;
	OverrideVars mSavedVars;

	Override();
	Override(OverrideParams *params, OverrideVars *vars, int transition, int release_transition,
		 TransitionType transition_type,
		 TransitionType release_transition_type,
		 bool is_conditional, CommandListExpression condition,
		 CommandList activate_command_list, CommandList deactivate_command_list) :
		transition(transition),
		release_transition(release_transition),
		transition_type(transition_type),
		release_transition_type(release_transition_type),
		is_conditional(is_conditional),
		condition(condition),
		activate_command_list(activate_command_list),
		deactivate_command_list(deactivate_command_list)
	{
		mOverrideParams = *params;
		mOverrideVars = *vars;
	}

	void ParseIniSection(LPCWSTR section) override;

	void Activate(PenguinDV *device, bool override_has_deactivate_condition);
	void Deactivate(PenguinDV *device);
	void Toggle(PenguinDV *device);
	bool MatchesCurrent(PenguinDV *device);
};

class KeyOverrideBase : public virtual OverrideBase, public InputListener
{
};

class KeyOverride : public KeyOverrideBase, public Override
{
private:
	KeyOverrideType type;

public:
	KeyOverride(KeyOverrideType type) :
		Override(),
		type(type)
	{}
	KeyOverride(KeyOverrideType type, OverrideParams *params, OverrideVars *vars,
			int transition, int release_transition,
			TransitionType transition_type,
			TransitionType release_transition_type,
			bool is_conditional, CommandListExpression condition,
			CommandList activate_command_list, CommandList deactivate_command_list) :
		Override(params, vars,
				transition, release_transition,
				transition_type, release_transition_type,
				is_conditional, condition,
				activate_command_list, deactivate_command_list),
		type(type)
	{}

	void DownEvent(PenguinDV *device);
	void UpEvent(PenguinDV *device);
#pragma warning(suppress : 4250) // Suppress ParseIniSection inheritance via dominance warning
};

class KeyOverrideCycle : public KeyOverrideBase
{
private:
	std::vector<class KeyOverride> presets;
	int current;
	bool wrap;
	bool smart;
public:
	KeyOverrideCycle() :
		current(-1),
		wrap(true),
		smart(true)
	{}

	void ParseIniSection(LPCWSTR section) override;
	void DownEvent(PenguinDV *device);
	void BackEvent(PenguinDV *device);
	void UpdateCurrent(PenguinDV *device);
};

class KeyOverrideCycleBack : public InputListener
{
	shared_ptr<KeyOverrideCycle> cycle;
public:
	KeyOverrideCycleBack(shared_ptr<KeyOverrideCycle> cycle) :
		cycle(cycle)
	{}

	void DownEvent(PenguinDV *device);
};

class PresetOverride : public Override
{
private:
	bool triggered;
	bool excluded;
	unordered_set<CommandListCommand*> triggers_this_frame;

public:
	PresetOverride() :
		Override(),
		triggered(false),
		excluded(false),
		unique_triggers_required(0)
	{}

	void Trigger(CommandListCommand *triggered_from);
	void Exclude();
	void Update(PenguinDV *device);

	unsigned unique_triggers_required;
};

// Sorted map so that if multiple presets affect the same thing the results
// will be consistent:
typedef std::map<std::wstring, class PresetOverride> PresetOverrideMap;
extern PresetOverrideMap presetOverrides;

struct OverrideTransitionParam
{
	float start;
	float target;
	ULONGLONG activation_time;
	int time;
	TransitionType transition_type;

	OverrideTransitionParam() :
		start(FLT_MAX),
		target(FLT_MAX),
		activation_time(0),
		time(-1),
		transition_type(TransitionType::LINEAR)
	{}
};

class OverrideTransition
{
public:
	std::map<OverrideParam, OverrideTransitionParam> params;
	std::map<CommandListVariable*, OverrideTransitionParam> vars;

	void ScheduleTransition(PenguinDV *wrapper,
			OverrideParams *targets, OverrideVars *vars,
			int time, TransitionType transition_type);
	void UpdatePresets(PenguinDV *wrapper);
	void OverrideTransition::UpdateTransitions(PenguinDV *wrapper);
	void Stop();
};

// This struct + class provides a global save for each of the overridable
// parameters. It is used to ensure that after all toggle and hold type
// bindings are released that the final value that is restored matches the
// original value. The local saves in each individual override do not guarantee
// this.
class OverrideGlobalSaveParam
{
private:
	float save;
	int refcount;
public:
	OverrideGlobalSaveParam();

	float Reset();
	void Save(float val);
	int Restore(float *val);
};

class OverrideGlobalSave
{
public:
	std::map<OverrideParam, OverrideGlobalSaveParam> params;
	std::map<CommandListVariable*, OverrideGlobalSaveParam> vars;

	void Reset(PenguinDV* wrapper);
	void Save(PenguinDV *wrapper, Override *preset);
	void Restore(Override *preset);
};

// We only use a single transition instance to simplify the edge cases and
// answer what happens when we have overlapping transitions - there can only be
// one active transition for each parameter.
extern OverrideTransition CurrentTransition;
extern OverrideGlobalSave OverrideSave;
