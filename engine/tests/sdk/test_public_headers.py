"""Compile the SDK as an external consumer, with no engine/src include path."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--include', action='append', default=[])
    parser.add_argument('--opencv', action='append', default=[])
    parser.add_argument('--projects', type=Path, required=True)
    args = parser.parse_args()
    flags = [args.compiler, '-std=c++11', '-fsyntax-only', '-x', 'c++']
    for directory in args.include:
        flags += ['-I', directory]
    for directory in args.opencv:
        flags += ['-isystem', directory]

    def compile_case(name, code, accepted=True, language='c++'):
        command = flags if language == 'c++' else [
            args.compiler, '-fsyntax-only', '-x', 'c',
            *[flag for directory in args.include for flag in ('-I', directory)],
        ]
        result = subprocess.run(command + ['-'], input=code, text=True, capture_output=True)
        if (result.returncode == 0) != accepted:
            raise RuntimeError(f'{name}: unexpected compiler result\n{result.stderr}')

    include = Path(args.include[0])
    headers = sorted(include.glob('*.h'))
    if not headers:
        raise RuntimeError(f'No public headers found in {include}')

    # Check resolved dependencies, including relative/absolute includes that can
    # bypass the absence of engine/src in the compiler's search path.
    include = include.resolve()
    repository = include.parent.parent
    projects = args.projects.resolve()
    public_code = ''.join(f'#include <{header.name}>\n' for header in headers)

    def check_dependencies(source, code=None, business=False):
        command = [flag for flag in flags if flag != '-fsyntax-only']
        result = subprocess.run(command + ['-MM', '-MT', 'sdk', str(source)],
                                input=code, text=True, capture_output=True, check=True)
        dependencies = shlex.split(result.stdout.partition(':')[2].replace('\\\n', ''))
        for dependency in dependencies:
            path = Path(dependency).resolve()
            if include in path.parents or (business and projects in path.parents):
                continue
            if repository in path.parents or projects in path.parents:
                raise RuntimeError(f'{source}: SDK boundary crossed through {path}')

    check_dependencies('-', public_code)
    business_sources = sorted(path for directory in ('modules', 'global_modules')
                              for path in (projects / directory).rglob('*')
                              if path.suffix in ('.cpp', '.cc', '.cxx'))
    if not business_sources:
        raise RuntimeError(f'No business sources found in {projects}')
    for source in business_sources:
        check_dependencies(source, business=True)
    for header in headers:
        compile_case(header.name, f'#include <{header.name}>\n')

    compile_case('business consumer', '''
#include <channel.h>
#include <events.h>
#include <global.h>
#include <control.h>
#include <gpio.h>
#include <json.h>
static void sdk_channel(ChannelContext *ctx) {
    struct State { int calls = 0; };
    if (auto *state = ctx->get_state<State>()) ++state->calls;
    ctx->publish_int("count", roi_count_target(ctx, "person", ROI_ALL));
    TargetQuery query;
    query.labels = {"person", "car"};
    query.min_score = 0.7f;
    query.roi = roi_find(ctx, "entrance");
    query.anchor = TargetAnchor::BottomCenter;
    const auto selected = ctx->query_targets(query);
    if (selected.valid() && selected.best) draw_rect(ctx, selected.best->box);
    (void)target_query_status_name(selected.status);
    draw_text(ctx, "test", ctx->business_center());
    ChannelFrameSnapshot evidence;
    ctx->get_channel_frame_snapshot(ctx->chnId, &evidence);
    const float timeout = ctx->param_float("timeout");
    (void)timeout;
    if (ctx->config) (void)ctx->config->models.size();
    EventRequest request;
    request.event_type = "test";
    request.fields = {event_field("count", ctx->target_count("person"))};
    report_event(ctx, request);
}
static void sdk_global(GlobalContext *ctx) {
    for (const auto &channel : ctx->inputs()) (void)channel.get_int("count");
    ChannelFrameSnapshot evidence;
    ctx->get_channel_frame_snapshot(0, &evidence);
    logic_control_set_channel_inference(0, true);
}
REGISTER_LOGIC(sdk_channel);
REGISTER_GLOBAL_LOGIC(sdk_global);
''')

    private_members = [
        'ChannelContext ctx{}; ctx.model_frame_getter = nullptr;',
        'ChannelContext ctx{}; ctx.draw_cmds = nullptr;',
        'ChannelContext ctx{}; ctx.logic_parameters = nullptr;',
        'GlobalContext ctx{}; ctx.ready_inputs = nullptr;',
        'ChannelLogicSnapshot snapshot; snapshot.media.reset();',
    ]
    for member in private_members:
        compile_case(member, '#include <global.h>\nvoid test() {' + member + '}\n', accepted=False)
    compile_case('global control is hidden', '#include <channel.h>\nAPP_CTRL *ctrl = g_pCtrl;\n', accepted=False)
    compile_case('selected target is read-only',
                 '#include <channel.h>\nvoid test(ChannelContext &ctx) { ctx.query_targets().best->score = 0; }\n',
                 accepted=False)
    compile_case('internal headers unavailable', '#include <runtime/app_ctrl.h>\n', accepted=False)
    for symbol in ('channel_logic_get', 'channel_logic_action_get', 'channel_logic_names',
                   'global_logic_get', 'global_logic_action_get', 'global_logic_names',
                   'draw_text_unicode_cached', 'gpio_init', 'gpio_deinit', 'gpio_restore_outputs'):
        compile_case(f'{symbol} is engine-only', public_code + f'auto hidden = &{symbol};\n', accepted=False)
    for code in ('RenderParams params;', 'LogicEntry entry;', 'GPIOCfg_t config;',
                 'void test(ChannelContext &ctx) { ctx.render_params(); }'):
        compile_case('internal rendering/registry/lifecycle types', public_code + code, accepted=False)
    compile_case('C GPIO and JSON API', '#include <gpio.h>\n#include <json.h>\n', language='c')
    # Execute the convenience API without any engine implementation or hardware.
    state_code = r'''
#include <channel.h>
#include <global.h>
#include <cassert>
struct State {
    static int live;
    int value;
    explicit State(int initial = 0) : value(initial) { ++live; }
    ~State() { --live; }
};
int State::live = 0;
int main() {
    ChannelContext unbound;
    assert(unbound.get_state<State>() == nullptr);
    assert(State::live == 0);
    {
        std::shared_ptr<void> first, second, global;
        ChannelContext a{}, b{};
        GlobalContext g{};
        a.state = &first; b.state = &second; g.state = &global;
        State *initial = a.get_state<State>(7);
        initial->value = 9;
        assert(a.get_state<State>(100) == initial);
        assert(a.get_state<State>()->value == 9);
        assert(b.get_state<State>()->value == 0);
        assert(g.get_state<State>(3)->value == 3);
        assert(g.get_state<State>(100)->value == 3);
        assert(State::live == 3);
        first.reset();
        assert(State::live == 2);
        assert(a.get_state<State>(4)->value == 4);
    }
    assert(State::live == 0);
}
'''
    with tempfile.TemporaryDirectory(prefix='vision-sdk-state-') as temporary:
        executable = Path(temporary) / 'state_test'
        command = [flag for flag in flags if flag != '-fsyntax-only']
        subprocess.run(command + ['-', '-o', str(executable)], input=state_code,
                       text=True, capture_output=True, check=True)
        subprocess.run([str(executable)], check=True)
    print(f'{len(headers)} standalone headers, {len(business_sources)} business dependency graphs, '
          'state lifecycle and SDK boundaries passed')


if __name__ == '__main__':
    main()
