// Prints what a replay says about its game mode: header properties, game event / ball archetypes, mutator-like attributes.
use boxcars::{Attribute, HeaderProp};
use std::collections::BTreeMap;

fn main() {
    let path = std::env::args().nth(1).expect("replay path");
    let data = std::fs::read(&path).expect("read");
    let replay = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse().expect("parse");
    for (k, v) in &replay.properties {
        if let HeaderProp::Array(_) = v { continue; }
        println!("header {k} = {v:?}");
    }
    let objects = &replay.objects;
    let mut seen = BTreeMap::new();
    let net = replay.network_frames.as_ref().unwrap();
    for f in &net.frames {
        for a in &f.new_actors {
            let name = &objects[usize::from(a.object_id)];
            if name.contains("GameEvent") || name.contains("Ball") || name.contains("Mutator") || name.contains("GameInfo") {
                *seen.entry(name.clone()).or_insert(0) += 1;
            }
        }
        for u in &f.updated_actors {
            let name = &objects[usize::from(u.object_id)];
            let interesting = name.contains("Scale") || name.contains("Mutator") || name.contains("GameEvent_Soccar_TA:")
                || name.contains("RechargeDelay") || name.contains("UnlimitedBoost") || name.contains("BoostModifier")
                || name.contains("GameEvent_TA:") || name.contains("GRI_X") || name.contains("Gravity");
            if interesting && !name.contains("SecondsRemaining") && !name.contains("ReplicatedServerPerformanceState") {
                let key = format!("{name} = {:?}", match &u.attribute { Attribute::Int(i) => format!("{i}"), Attribute::Float(x) => format!("{x}"), Attribute::Byte(b) => format!("{b}"), Attribute::Boolean(b) => format!("{b}"), other => format!("{other:?}").chars().take(120).collect() });
                *seen.entry(key).or_insert(0) += 1;
            }
        }
    }
    for (k, n) in seen { println!("{n:5}  {k}"); }
}
