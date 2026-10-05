use crate::{Error, Result, xml::escape};
#[derive(Debug, Clone)]
pub struct Speaker {
    pub label: String,
    pub azimuth: f32,
    pub elevation: f32,
    pub lfe: bool,
    pub azimuth_range: Option<(f32, f32)>,
    pub elevation_range: Option<(f32, f32)>,
}
#[derive(Debug, Clone)]
pub struct Chna {
    pub track: u16,
    pub uid: String,
    pub format: String,
    pub pack: String,
}
#[derive(Debug, Clone)]
pub struct GeneratedMetadata {
    pub axml: String,
    pub chna: Vec<Chna>,
}

pub fn generate(kind: u32, name: &str, speakers: &[Speaker]) -> Result<GeneratedMetadata> {
    let (definition, count, object_name) = match kind {
        1 => ("DirectSpeakers", speakers.len(), format!("{name} Render")),
        4 => ("HOA", 16, "HOA3 Render".to_owned()),
        5 => ("Binaural", 2, "Binaural Render".to_owned()),
        _ => return Err(Error::invalid("不支持的 ADM 输出类型")),
    };
    if count == 0 || count > u16::MAX as usize {
        return Err(Error::invalid("ADM 输出声道数量无效"));
    }
    let pack = format!("AP_{kind:04x}1001");
    let mut xml = String::from(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ebuCoreMain xmlns=\"urn:ebu:metadata-schema:ebuCore_2017\"><coreMetadata><format><audioFormatExtended version=\"ITU-R_BS.2076-2\">\n",
    );
    xml.push_str(&format!("<audioProgramme audioProgrammeID=\"APR_1001\" audioProgrammeName=\"{} Programme\"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme>\n<audioContent audioContentID=\"ACO_1001\" audioContentName=\"{} Content\"><audioObjectIDRef>AO_1001</audioObjectIDRef></audioContent>\n<audioObject audioObjectID=\"AO_1001\" audioObjectName=\"{}\"><audioPackFormatIDRef>{pack}</audioPackFormatIDRef>",escape(&object_name),escape(&object_name),escape(&object_name)));
    for i in 0..count {
        xml.push_str(&format!(
            "<audioTrackUIDRef>ATU_{:08x}</audioTrackUIDRef>",
            i + 1
        ));
    }
    xml.push_str("</audioObject>\n");
    xml.push_str(&format!("<audioPackFormat audioPackFormatID=\"{pack}\" audioPackFormatName=\"{}\" typeLabel=\"{kind:04x}\" typeDefinition=\"{definition}\">",escape(name)));
    if kind == 4 {
        xml.push_str("<normalization>SN3D</normalization>");
    }
    for i in 0..count {
        xml.push_str(&format!(
            "<audioChannelFormatIDRef>AC_{kind:04x}{:04x}</audioChannelFormatIDRef>",
            0x1001 + i
        ));
    }
    xml.push_str("</audioPackFormat>\n");
    let mut chna = Vec::new();
    for i in 0..count {
        let suffix = format!("{kind:04x}{:04x}", 0x1001 + i);
        let channel_name = match kind {
            1 => format!("{} {}", speakers[i].label, i + 1),
            4 => format!("ACN {i}"),
            _ => ["leftEar", "rightEar"][i].to_owned(),
        };
        let channel_name = escape(&channel_name);
        xml.push_str(&format!("<audioChannelFormat audioChannelFormatID=\"AC_{suffix}\" audioChannelFormatName=\"{channel_name}\" typeLabel=\"{kind:04x}\" typeDefinition=\"{definition}\">"));
        if kind == 1 && speakers[i].lfe {
            xml.push_str("<frequency typeDefinition=\"lowPass\">120</frequency>");
        }
        xml.push_str(&format!(
            "<audioBlockFormat audioBlockFormatID=\"AB_{suffix}_00000001\">"
        ));
        match kind {
            1 => {
                let speaker = &speakers[i];
                xml.push_str(&format!(
                    "<speakerLabel>{}</speakerLabel>",
                    escape(&speaker.label)
                ));
                for (coordinate, value, range) in [
                    ("azimuth", speaker.azimuth, speaker.azimuth_range),
                    ("elevation", speaker.elevation, speaker.elevation_range),
                    ("distance", 1.0, None),
                ] {
                    let bounds = match coordinate {
                        "azimuth" => (-180.0, 180.0),
                        "elevation" => (-90.0, 90.0),
                        _ => (0.0, f32::MAX),
                    };
                    if !value.is_finite() || !(bounds.0..=bounds.1).contains(&value) {
                        return Err(Error::invalid("扬声器坐标必须有限"));
                    }
                    xml.push_str(&format!(
                        "<position coordinate=\"{coordinate}\">{value}</position>"
                    ));
                    if let Some((lo, hi)) = range {
                        if !lo.is_finite()
                            || !hi.is_finite()
                            || lo > hi
                            || lo < bounds.0
                            || hi > bounds.1
                        {
                            return Err(Error::invalid("扬声器范围无效"));
                        }
                        xml.push_str(&format!("<position coordinate=\"{coordinate}\" bound=\"min\">{lo}</position><position coordinate=\"{coordinate}\" bound=\"max\">{hi}</position>"));
                    }
                }
            }
            4 => {
                let order = (i as f64).sqrt() as i32;
                let degree = i as i32 - order * (order + 1);
                xml.push_str(&format!("<order>{order}</order><degree>{degree}</degree><normalization>SN3D</normalization>"));
            }
            _ => {}
        }
        xml.push_str("</audioBlockFormat></audioChannelFormat>\n");
        xml.push_str(&format!("<audioStreamFormat audioStreamFormatID=\"AS_{suffix}\" audioStreamFormatName=\"{channel_name} Stream\" formatLabel=\"0001\" formatDefinition=\"PCM\"><audioChannelFormatIDRef>AC_{suffix}</audioChannelFormatIDRef><audioTrackFormatIDRef>AT_{suffix}_01</audioTrackFormatIDRef></audioStreamFormat>\n<audioTrackFormat audioTrackFormatID=\"AT_{suffix}_01\" audioTrackFormatName=\"{channel_name} Track\" formatLabel=\"0001\" formatDefinition=\"PCM\"><audioStreamFormatIDRef>AS_{suffix}</audioStreamFormatIDRef></audioTrackFormat>\n<audioTrackUID UID=\"ATU_{:08x}\"><audioTrackFormatIDRef>AT_{suffix}_01</audioTrackFormatIDRef><audioPackFormatIDRef>{pack}</audioPackFormatIDRef></audioTrackUID>\n",i+1));
        chna.push(Chna {
            track: (i + 1) as u16,
            uid: format!("ATU_{:08x}", i + 1),
            format: format!("AT_{suffix}_01"),
            pack: pack.clone(),
        });
    }
    xml.push_str("</audioFormatExtended></format></coreMetadata></ebuCoreMain>\n");
    Ok(GeneratedMetadata { axml: xml, chna })
}
