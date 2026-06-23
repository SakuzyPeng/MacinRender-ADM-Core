using System;
using System.IO;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace MacinRender.Gui.Services;

// Minecraft 风格皮肤像素源:标准 64×64 ARGB 纹理。默认程序化生成(原创配色,非 Mojang 官方);
// 阶段 2 将支持由导入的 64×64 PNG 替换。绘制层按各部位/面的标准 UV 区域采样此纹理。
public sealed class CharacterSkin
{
    public const int Tex = 64;
    private readonly uint[] _px; // ARGB8888,长度 Tex*Tex

    public CharacterSkin(uint[] pixels, bool slim = false)
    {
        if (pixels.Length != Tex * Tex)
        {
            throw new ArgumentException($"皮肤纹理必须是 {Tex}×{Tex}", nameof(pixels));
        }

        _px = pixels;
        IsSlim = slim;
    }

    // true = Alex(纤细,手臂 3 宽);false = Steve(经典,4 宽)。
    public bool IsSlim { get; }

    // 启发判定 Alex:经典右臂在皮肤上占 x40..55,Alex 只占 40..53 → x54/55(y20..31)经典不透明、Alex 透明。
    private static bool IsSlimSkin(uint[] px)
    {
        for (int y = 20; y < 32; y++)
        {
            for (int x = 54; x <= 55; x++)
            {
                if ((px[(y * Tex) + x] >> 24) != 0)
                {
                    return false; // 这两列有内容 → 经典(4 宽臂)
                }
            }
        }

        return true;
    }

    // 取像素 ARGB;越界或全透明返回 0(alpha=0,绘制层跳过)。
    public uint At(int x, int y) => (uint)x >= Tex || (uint)y >= Tex ? 0u : _px[(y * Tex) + x];

    public static CharacterSkin Default { get; } = BuildDefault();

    // 从标准 64×64 PNG 皮肤加载(取左上 64×64;BGRA→ARGB)。失败返回 null(静默,不阻断)。
    public static CharacterSkin? LoadFromPng(string path)
    {
        try
        {
            using var bmp = new Bitmap(path);
            int w = bmp.PixelSize.Width;
            int h = bmp.PixelSize.Height;
            if (w < Tex || h < Tex)
            {
                return null; // 至少 64×64(老版 64×32 暂不支持)
            }

            int stride = w * 4;
            var buf = new byte[h * stride];
            var handle = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                bmp.CopyPixels(new PixelRect(0, 0, w, h), handle.AddrOfPinnedObject(), buf.Length, stride);
            }
            finally
            {
                handle.Free();
            }

            // 字节顺序按解码格式判定:macOS Skia 解码为 Rgba8888,其它平台可能 Bgra8888。
            bool bgra = bmp.Format == PixelFormats.Bgra8888;
            var px = new uint[Tex * Tex];
            for (int y = 0; y < Tex; y++)
            {
                for (int x = 0; x < Tex; x++)
                {
                    int o = (y * stride) + (x * 4);
                    byte a = buf[o + 3];
                    byte g = buf[o + 1];
                    byte r = bgra ? buf[o + 2] : buf[o];
                    byte b = bgra ? buf[o] : buf[o + 2];
                    px[(y * Tex) + x] = ((uint)a << 24) | ((uint)r << 16) | ((uint)g << 8) | b;
                }
            }

            return new CharacterSkin(px, IsSlimSkin(px));
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or InvalidOperationException)
        {
            return null;
        }
    }

    // 程序化默认皮肤(原创配色,Minecraft 像素风):各部位面填底色,头正面补五官。
    private static CharacterSkin BuildDefault()
    {
        var px = new uint[Tex * Tex];

        const uint skin = 0xFFBB8B61;
        const uint skinDark = 0xFF9C704C;
        const uint hair = 0xFF45301E;
        const uint shirt = 0xFF3A6EA5;
        const uint shirtDark = 0xFF2E5680;
        const uint pants = 0xFF2E3A59;
        const uint shoe = 0xFF50555E;
        const uint eyeWhite = 0xFFE6E6E6;
        const uint iris = 0xFF4A5B8C;
        const uint mouth = 0xFF6B4A38;

        void Fill(int x, int y, int w, int h, uint c)
        {
            for (int yy = y; yy < y + h; yy++)
            {
                for (int xx = x; xx < x + w; xx++)
                {
                    px[(yy * Tex) + xx] = c;
                }
            }
        }

        void Px(int x, int y, uint c) => px[(y * Tex) + x] = c;

        // ── 头(8 立方,标准布局)──
        Fill(8, 0, 8, 8, hair);    // top 头顶头发
        Fill(16, 0, 8, 8, skin);   // bottom 下巴/脖
        Fill(0, 8, 8, 8, skin);    // right 脸颊
        Fill(0, 8, 8, 3, hair);    // right 上部侧发
        Fill(16, 8, 8, 8, skin);   // left 脸颊
        Fill(16, 8, 8, 3, hair);   // left 上部侧发
        Fill(8, 8, 8, 8, skin);    // front 底肤色(下补五官)
        Fill(24, 8, 8, 8, hair);   // back 后脑头发

        // 头正面五官(front 区原点 8,8,8×8;col=x-8、row=y-8,左右对称,中心 col 3.5)。
        Fill(8, 8, 8, 1, hair);          // 发际(row0)
        Fill(8, 8, 1, 3, hair);          // 左鬓角(col0)
        Fill(15, 8, 1, 3, hair);         // 右鬓角(col7)
        Fill(9, 9, 2, 1, hair);          // 左眉(col1-2)
        Fill(13, 9, 2, 1, hair);         // 右眉(col5-6)
        Px(9, 10, eyeWhite);
        Px(10, 10, iris);                // 左眼:外白 col1、内瞳 col2
        Px(13, 10, iris);
        Px(14, 10, eyeWhite);            // 右眼:内瞳 col5、外白 col6
        Px(11, 12, skinDark);
        Px(12, 12, skinDark);            // 鼻(col3-4)
        Fill(10, 14, 4, 1, mouth);       // 嘴(col2-5)

        // ── 身体(8×12×4)──
        Fill(20, 16, 8, 4, shirt);   // top
        Fill(28, 16, 8, 4, shirtDark); // bottom
        Fill(16, 20, 4, 12, shirtDark); // right
        Fill(20, 20, 8, 12, shirt);  // front
        Fill(28, 20, 4, 12, shirtDark); // left
        Fill(32, 20, 8, 12, shirt);  // back

        // ── 右臂(4×12×4)──
        Fill(44, 16, 4, 4, skin);
        Fill(48, 16, 4, 4, skin);
        Fill(40, 20, 4, 12, skinDark);
        Fill(44, 20, 4, 12, skin);
        Fill(48, 20, 4, 12, skin);
        Fill(52, 20, 4, 12, skinDark);

        // ── 左臂(4×12×4,64×64 专属区)──
        Fill(36, 48, 4, 4, skin);
        Fill(40, 48, 4, 4, skin);
        Fill(32, 52, 4, 12, skinDark);
        Fill(36, 52, 4, 12, skin);
        Fill(40, 52, 4, 12, skin);
        Fill(44, 52, 4, 12, skinDark);

        // ── 右腿(4×12×4)──
        Fill(4, 16, 4, 4, pants);
        Fill(8, 16, 4, 4, shoe);   // 脚底
        Fill(0, 20, 4, 12, pants);
        Fill(4, 20, 4, 12, pants);
        Fill(8, 20, 4, 12, pants);
        Fill(12, 20, 4, 12, pants);
        Fill(4, 28, 4, 4, shoe);   // front 下段当鞋
        Fill(0, 28, 4, 4, shoe);
        Fill(8, 28, 4, 4, shoe);
        Fill(12, 28, 4, 4, shoe);

        // ── 左腿(4×12×4,64×64 专属区)──
        Fill(20, 48, 4, 4, pants);
        Fill(24, 48, 4, 4, shoe);
        Fill(16, 52, 4, 12, pants);
        Fill(20, 52, 4, 12, pants);
        Fill(24, 52, 4, 12, pants);
        Fill(28, 52, 4, 12, pants);
        Fill(20, 60, 4, 4, shoe);
        Fill(16, 60, 4, 4, shoe);
        Fill(24, 60, 4, 4, shoe);
        Fill(28, 60, 4, 4, shoe);

        return new CharacterSkin(px);
    }
}
