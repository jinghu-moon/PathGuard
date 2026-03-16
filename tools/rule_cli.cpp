#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "rule_engine.h"

namespace {

struct Options {
    std::string rules_path = "config/rules.ini";
    std::string process_name;
    std::vector<std::string> paths;
    std::string operation = "open";
    int open_flags = 0;
};

void PrintUsage() {
    std::cout
        << "Usage: fm_rule_cli --process <pkg> --path <path> [--path <path> ...] [options]\n"
        << "Options:\n"
        << "  --rules <file>      rules.ini 路径（默认 config/rules.ini）\n"
        << "  --process <pkg>     目标包名/进程名\n"
        << "  --path <path>       测试路径，可重复\n"
        << "  --op <op>           操作类型: open|open_dir|create_file|create_dir|stat|list|readlink|remove_file|remove_dir\n"
        << "  --flags <num>       open/openat flags（默认 0，可写 0x）\n"
        << "  --help              显示帮助\n";
}

bool ParseArgs(int argc, char **argv, Options *out) {
    if (out == nullptr) {
        return false;
    }

    for (int index = 1; index < argc; ++index) {
        std::string_view arg = argv[index];
        if (arg == "--help") {
            PrintUsage();
            return false;
        }
        if (arg == "--rules" && index + 1 < argc) {
            out->rules_path = argv[++index];
            continue;
        }
        if (arg == "--process" && index + 1 < argc) {
            out->process_name = argv[++index];
            continue;
        }
        if (arg == "--path" && index + 1 < argc) {
            out->paths.emplace_back(argv[++index]);
            continue;
        }
        if (arg == "--op" && index + 1 < argc) {
            out->operation = argv[++index];
            continue;
        }
        if (arg == "--flags" && index + 1 < argc) {
            out->open_flags = static_cast<int>(std::strtol(argv[++index], nullptr, 0));
            continue;
        }
        std::cerr << "未知参数: " << arg << "\n";
        return false;
    }

    if (out->process_name.empty() || out->paths.empty()) {
        std::cerr << "缺少 --process 或 --path\n";
        return false;
    }

    return true;
}

bool BuildRuntimeContext(const Options &options, fm::RuntimeContext *context) {
    if (context == nullptr) {
        return false;
    }

    fm::RuntimeContext ctx;
    ctx.open_flags = options.open_flags;

    if (options.operation == "open") {
        ctx.operation = fm::PathOperation::kOpen;
    } else if (options.operation == "open_dir") {
        ctx.operation = fm::PathOperation::kOpenDirectory;
    } else if (options.operation == "create_file") {
        ctx.operation = fm::PathOperation::kCreateFile;
    } else if (options.operation == "create_dir") {
        ctx.operation = fm::PathOperation::kCreateDirectory;
    } else if (options.operation == "stat") {
        ctx.operation = fm::PathOperation::kStat;
    } else if (options.operation == "list") {
        ctx.operation = fm::PathOperation::kEnumerateDirectory;
    } else if (options.operation == "readlink") {
        ctx.operation = fm::PathOperation::kReadLink;
    } else if (options.operation == "remove_file") {
        ctx.operation = fm::PathOperation::kRemoveFile;
    } else if (options.operation == "remove_dir") {
        ctx.operation = fm::PathOperation::kRemoveDirectory;
    } else {
        return false;
    }

    *context = ctx;
    return true;
}

const char *DecisionName(fm::MatchDecision decision) {
    switch (decision) {
        case fm::MatchDecision::kAllow:
            return "allow";
        case fm::MatchDecision::kBlock:
            return "block";
        case fm::MatchDecision::kRedirect:
            return "redirect";
        case fm::MatchDecision::kNoMatch:
        default:
            return "no_match";
    }
}

const char *ActionName(fm::RuleAction action) {
    switch (action) {
        case fm::RuleAction::kAllow:
            return "allow";
        case fm::RuleAction::kDeny:
            return "deny";
        case fm::RuleAction::kRedirect:
            return "redirect";
        case fm::RuleAction::kRedirectDynamic:
            return "redirect_dynamic";
        case fm::RuleAction::kDelete:
            return "delete";
        default:
            return "unknown";
    }
}

const char *KindName(fm::PathKind kind) {
    switch (kind) {
        case fm::PathKind::kFile:
            return "file";
        case fm::PathKind::kDirectory:
            return "dir";
        case fm::PathKind::kAuto:
        default:
            return "auto";
    }
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        return 1;
    }

    std::ifstream input(options.rules_path);
    if (!input) {
        std::cerr << "无法读取规则文件: " << options.rules_path << "\n";
        return 1;
    }

    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    fm::ParsedRules parsed;
    if (!fm::ParseRulesIni(text, &parsed)) {
        std::cerr << "规则解析失败\n";
        for (const auto &error : parsed.errors) {
            std::cerr << "  - " << error << "\n";
        }
        return 1;
    }

    fm::AppPolicy policy;
    std::string error;
    if (!fm::CompilePolicyForProcess(parsed, options.process_name, {}, &policy, &error)) {
        std::cerr << "规则编译失败: " << error << "\n";
        return 1;
    }

    fm::RuntimeContext context;
    if (!BuildRuntimeContext(options, &context)) {
        std::cerr << "未知操作类型: " << options.operation << "\n";
        return 1;
    }

    fm::ResolvedPathKindCache cache;
    for (const auto &path : options.paths) {
        fm::MatchResult result = fm::MatchPath(policy, path, context, &cache);
        std::cout << "path: " << path << "\n";
        std::cout << "decision: " << DecisionName(result.decision) << "\n";
        std::cout << "resolved_kind: " << KindName(result.resolved_kind) << "\n";
        if (!result.redirect_path.empty()) {
            std::cout << "redirect: " << result.redirect_path << "\n";
        }
        if (result.matched_rule != nullptr) {
            std::cout << "rule_index: " << result.matched_rule_index << "\n";
            std::cout << "rule_line: " << result.matched_rule->line_number << "\n";
            std::cout << "rule_action: " << ActionName(result.matched_rule->action) << "\n";
            std::cout << "rule_path: " << result.matched_rule->path << "\n";
            if (!result.matched_rule->extensions.empty()) {
                std::cout << "rule_types: ";
                for (size_t i = 0; i < result.matched_rule->extensions.size(); ++i) {
                    if (i > 0) {
                        std::cout << ",";
                    }
                    std::cout << result.matched_rule->extensions[i];
                }
                std::cout << "\n";
            }
        }
        std::cout << "---\n";
    }

    return 0;
}
