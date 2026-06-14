using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Models;
using MacinRender.Gui.Services;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui;

/// <summary>
/// headless 桥接层自检(不进 UI):验证 dylib 加载 + P/Invoke + UTF-8 字符串 + 释放链路,
/// 给出 wav 路径时再跑一次真实渲染,验证结构化进度回调 + 取消桥 + result 读取。
///   dotnet run -c Release --project gui/MacinRender.Gui -- --selftest [adm.wav]
/// </summary>
internal static class SelfTest
{
    public static async Task<int> Run(string? inputWav)
    {
        Console.WriteLine("== MacinRender 桥接层自检 ==");

        try
        {
            Console.WriteLine(
                $"C ABI 版本: v{NativeMethods.adm_api_version_major()}." +
                $"{NativeMethods.adm_api_version_minor()}.{NativeMethods.adm_api_version_patch()}");
        }
        catch (DllNotFoundException ex)
        {
            Console.Error.WriteLine($"[失败] 找不到 libmradm_capi —— 先跑 gui/copy-native.sh。{ex.Message}");
            return 2;
        }

        using var ctx = NativeMethods.adm_create_context();
        if (ctx.IsInvalid)
        {
            Console.Error.WriteLine("[失败] adm_create_context 返回 NULL");
            return 3;
        }

        // 查询链路:JSON-out + adm_free_string。
        var rc = NativeMethods.adm_output_formats_json(ctx, out var jsonPtr);
        if (rc == AdmErrorCode.Ok && jsonPtr != IntPtr.Zero)
        {
            string json = Marshal.PtrToStringUTF8(jsonPtr) ?? string.Empty;
            NativeMethods.adm_free_string(jsonPtr);
            Console.WriteLine($"output_formats_json: {json.Length} 字符");
            Console.WriteLine("  " + (json.Length > 280 ? json[..280] + "…" : json));
        }
        else
        {
            Console.Error.WriteLine($"[失败] adm_output_formats_json: {rc}");
            return 4;
        }

        // 模型构建验证:support-matrix → OutputModel(GUI 启动走同一路径)。
        SupportMatrixDoc? matrix;
        try
        {
            matrix = AdmQueries.LoadSupportMatrix(ctx);
        }
        catch (EntryPointNotFoundException ex)
        {
            Console.Error.WriteLine($"[失败] libmradm_capi 缺少 adm_render_support_matrix_json: {ex.Message}");
            return 7;
        }

        if (matrix is null)
        {
            Console.Error.WriteLine("[失败] adm_render_support_matrix_json 未返回有效矩阵");
            return 7;
        }

        OutputModel.Initialize(matrix);
        Console.WriteLine($"backends({OutputModel.Backends.Count}):");
        foreach (var b in OutputModel.Backends.Where(b => b.Id != "automatic"))
        {
            Console.WriteLine($"  {b.Id} \"{b.Name}\" (renderer={b.Renderer}):");
            foreach (var lid in b.LayoutIds)
            {
                var l = OutputModel.LayoutById[lid];
                Console.WriteLine($"      [{lid}] \"{l.Name}\" {l.Channels}ch height={l.HasHeight}");
            }
        }
        Console.WriteLine($"codecs: {string.Join(", ", OutputModel.Codecs.Select(c => $"{c.Id}={(c.Available ? "on" : "off")}"))}");
        Console.WriteLine($"features: apac={OutputModel.ApacAvailable} iamf={OutputModel.IamfAvailable} sofa={OutputModel.SofaAvailable}");

        Console.WriteLine("联动链抽查 (后端 → 编码器 → 该编码器支持的布局):");
        void Chain(string backendId)
        {
            var backend = OutputModel.BackendById[backendId];
            foreach (var c in OutputModel.Codecs.Where(c => OutputModel.IsCodecSupportedByBackend(backend, c.Id)))
            {
                var lays = backend.LayoutIds.Where(l => OutputModel.IsCodecSupported(backendId, l, c.Id));
                Console.WriteLine($"  {backendId}/{c.Id} → [{string.Join(", ", lays)}]");
            }
        }

        Chain("ear");
        Chain("saf-binaural");

        if (string.IsNullOrEmpty(inputWav))
        {
            Console.WriteLine("(未给 wav,跳过真实渲染。加载/查询链路 OK。)");
            return 0;
        }

        if (!File.Exists(inputWav))
        {
            Console.Error.WriteLine($"[失败] 输入不存在: {inputWav}");
            return 5;
        }

        // 语义编辑路径(#13):inspect → 对象 + 听感维度当前值;策略模板可达性(#14 当前值权威源)。
        var inspectDoc = SemanticInspect.Parse(AdmQueries.FetchInspectJson(ctx, inputWav));
        if (inspectDoc is null)
        {
            Console.Error.WriteLine("[失败] adm_inspect_file_json / 解析失败");
            return 8;
        }

        Console.WriteLine($"语义 inspect: {inspectDoc.Objects.Count} 个对象");
        foreach (var o in inspectDoc.Objects.Take(3))
        {
            var item = SemanticObjectItem.From(o);
            Console.WriteLine($"  {item.Display} — {item.Summary}");
        }

        var tmpl = AdmQueries.FetchPolicyTemplateJson(ctx, inputWav);
        Console.WriteLine($"policy_template_json: {(tmpl is null ? "null" : tmpl.Length + " 字符")}");

        // 语义编辑器(#14):构造覆盖 → 拼 policy JSON → 喂核心校验可解析(相对变换:× scale / dB)。
        if (inspectDoc.Objects.Count > 0)
        {
            var item = SemanticObjectItem.From(inspectDoc.Objects[0]);
            item.DiffuseScale.Enabled = true;
            item.DiffuseScale.Value = 0.5;
            item.GainDb.Enabled = true;
            item.GainDb.Value = -3.0;
            var rule = item.BuildRule();
            var policy = new JsonObject
            {
                ["schema"] = "mradm.semantic-policy.v1",
                ["objects"] = new JsonArray(new JsonNode?[] { rule }),
            }.ToJsonString();
            Console.WriteLine($"编辑器 policy JSON: {policy}");

            using var policyOpts = NativeMethods.adm_create_render_options();
            NativeMethods.adm_render_options_set_renderer(policyOpts, AdmRenderer.Binaural);
            NativeMethods.adm_render_options_set_output_layout(policyOpts, "binaural");
            // setter 只保存字符串(malformed JSON 不在此诊断,见 c_api.h);Ok 仅证明 P/Invoke 可调。
            var setRc = NativeMethods.adm_render_options_set_semantic_policy_json(policyOpts, policy);
            Console.WriteLine($"set_semantic_policy_json(仅保存字符串): {setRc}");

            // 真验解析:create_preview_session 在创建时 import + apply 该 policy,JSON 解析/校验错误在此暴露。
            var psRc = NativeMethods.adm_create_preview_session(ctx, inputWav, policyOpts, out var session);
            using (session)
            {
                Console.WriteLine($"create_preview_session(应用 policy,真验解析): {psRc}");
                if (psRc != AdmErrorCode.Ok)
                {
                    Console.Error.WriteLine("[失败] 编辑器拼出的 policy JSON 在 preview session 创建时被核心拒绝");
                    return 9;
                }
            }
        }

        string outPath = Path.Combine(Path.GetTempPath(), "mradm_selftest.flac");
        var counter = new CountingProgress();
        var settings = new RenderSettings { Renderer = AdmRenderer.Binaural, Layout = "binaural" };

        Console.WriteLine($"渲染: {inputWav} → {outPath} (binaural/FLAC)");
        var svc = new AdmRenderService();
        var outcome = await svc.RenderAsync(inputWav, outPath, settings, counter, CancellationToken.None);

        Console.WriteLine($"进度回调命中: {counter.Count} 次 (最后 {counter.Last?.OverallFraction:P0} " +
                          $"@ {counter.Last?.Stage}/{counter.Last?.Operation})");
        Console.WriteLine($"结果: success={outcome.Success} code={outcome.ErrorCode} \"{outcome.Message}\"");
        Console.WriteLine($"  输出: {outcome.OutputPath}");
        Console.WriteLine($"  响度: {(outcome.LoudnessLufs is { } lu ? $"{lu:F1} LUFS" : "—")}  " +
                          $"峰值: {(outcome.PeakDbtp is { } pk ? $"{pk:F1} dBTP" : "—")}");
        Console.WriteLine($"  日志: {outcome.Logs.Count} 条");
        foreach (var log in outcome.Logs)
        {
            Console.WriteLine($"    [{log.Level}] {log.Module}: {log.Message}");
        }

        if (outcome.Success && outcome.OutputPath is { } path && File.Exists(path))
        {
            Console.WriteLine($"  文件大小: {new FileInfo(path).Length} 字节 ✓");
            return 0;
        }

        return outcome.Success ? 0 : 6;
    }

    private sealed class CountingProgress : IProgress<RenderProgress>
    {
        public int Count { get; private set; }
        public RenderProgress? Last { get; private set; }

        public void Report(RenderProgress value)
        {
            Count++;
            Last = value;
        }
    }
}
