//! `mr_adm_make_fixture <kind> <out.wav>`: writes one deterministic ADM BWF fixture of the
//! consistency matrix ([`mradm_testkit::fixtures`]).
use mradm_testkit::fixtures::fixture;
use std::path::Path;
use std::process::ExitCode;

fn usage() -> ExitCode {
    eprint!(
        "usage: mr_adm_make_fixture <kind> <out.wav>\n\
         kinds: objects-point | objects-extent | objects-extent-multi | objects-cartesian\n     \
         | objects-room | objects-room-96k | params | directspeakers | directspeakers-lfe\n     \
         | hoa | hoa3 | policy-objects\n"
    );
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        return usage();
    }
    let (kind, out) = (&args[1], &args[2]);
    let Some(f) = fixture(kind) else {
        return usage();
    };
    if let Err(error) = f.built.write(Path::new(out), f.seed, f.rate, f.frames) {
        eprintln!("fixture setup failed: {error}");
        return ExitCode::from(1);
    }
    println!(
        "wrote {out} kind={kind} channels={} frames={} rate={}",
        f.built.uids.len(),
        f.frames,
        f.rate
    );
    ExitCode::SUCCESS
}
