namespace MacinRender.Gui.Interop;

// 以下枚举严格镜像 include/adm/c_api.h 的数值,不得重排或改值。
// 头文件契约:已有值冻结,新值只在末尾追加(随 minor 版本)。

public enum AdmErrorCode
{
    Ok = 0,
    InvalidArgument = 1,
    Unsupported = 2,
    Io = 3,
    RenderFailed = 4,
    Cancelled = 5,
    Internal = 6,
}

public enum AdmRenderer
{
    Automatic = 0,
    Ear = 1,
    Saf = 2,
    Hoa = 3,
    Apple = 4,
    Binaural = 5,
    SafBinaural = 6,
}

public enum AdmOutputBitDepth
{
    F32 = 0,
    I24 = 1,
    I16 = 2,
}

public enum AdmSpeakerSpreadMode
{
    Automatic = 0,
    None = 1,
    Mdap = 2,
}

public enum AdmBinauralSpreadMode
{
    Automatic = 0,
    None = 1,
    Cloud = 2,
    SafSpreader = 3,
}

public enum AdmIamfContainer
{
    Obu = 0,
    Mp4 = 1,
}

public enum AdmApacContainer
{
    Mpeg4 = 0,
    Caf = 1,
}

public enum AdmLogLevel
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
}

public enum AdmRenderStage
{
    Unknown = 0,
    Validating = 1,
    Probing = 2,
    ImportingScene = 3,
    Planning = 4,
    Rendering = 5,
    PostProcessing = 6,
    Finished = 7,
}

public enum AdmProgressOperation
{
    Unknown = 0,
    ValidateRequest = 1,
    ProbeInput = 2,
    ImportScene = 3,
    ApplySemanticPolicy = 4,
    PlanRender = 5,
    PrepareBackend = 6,
    RenderAudio = 7,
    TrimOutput = 8,
    ApplyGain = 9,
    ConvertBitDepth = 10,
    EncodeFlac = 11,
    EncodeOpus = 12,
    EncodeApac = 13,
    EncodeIamf = 14,
    PackageIamfMp4 = 15,
    WriteMetadata = 16,
    Finish = 17,
}
