// m0dump: for every function of a metallib, its stage, properties (vertex attributes) and
// function constants, as one JSON object per line.
//   m0dump default.metallib > functions.jsonl
use metal2vulkan::library::Library;
use metal2vulkan::Budget;

fn main() {
    let path = std::env::args().nth(1).expect("usage: m0dump in.metallib");
    let bytes = std::fs::read(&path).expect("read");
    let lib = Library::parse(&bytes).expect("parse");
    for i in 0..lib.len() {
        let f = match lib.function_at(i) {
            Ok(f) => f,
            Err(e) => {
                println!("{}", serde_json::json!({"index": i, "error": format!("{e}")}));
                continue;
            }
        };
        let info = f.info();
        let name = format!("{:?}", info);
        let props = f.properties(&Budget::standard());
        let consts = f.function_constants_dictionary();
        let props = match props {
            Ok(p) => serde_json::to_value(&p).unwrap(),
            Err(e) => serde_json::json!({"error": format!("{e}")}),
        };
        let consts = match consts {
            Ok(c) => serde_json::to_value(&c).unwrap(),
            Err(e) => serde_json::json!({"error": format!("{e}")}),
        };
        println!(
            "{}",
            serde_json::json!({"index": i, "info": name, "properties": props, "constants": consts})
        );
    }
}
