#![feature(proc_macro_hygiene, decl_macro)]

#[macro_use]
extern crate rocket;
extern crate rocket_cors;
#[macro_use]
extern crate rocket_contrib;
#[macro_use]
extern crate serde_derive;

extern crate bincode;

use std::sync::Mutex;

use rocket_contrib::json::{Json, JsonValue};

use rocket::http::Method; // 1.

use rocket_cors::{
    AllowedHeaders, AllowedOrigins, Error, // 2.
    Cors, CorsOptions // 3.
};

fn make_cors() -> Cors {
    let allowed_origins = AllowedOrigins::some_exact(&[ // 4.
        "http://localhost:8080",
        "http://127.0.0.1:8080",
        "http://localhost:8000",
        "http://0.0.0.0:8000",
    ]);

    CorsOptions { // 5.
        allowed_origins,
        allowed_methods: vec![Method::Get, Method::Post].into_iter().map(From::from).collect(), // 1.
        allowed_headers: AllowedHeaders::some(&[
            "Authorization",
            "Accept",
            "Content-Type",
            "User-Agent",
            "Access-Control-Allow-Origin", // 6.
        ]),
        allow_credentials: true,
        ..Default::default()
    }
    .to_cors()
    .expect("error while building CORS")
}

type Data = Mutex<ChannelData>;

#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct ChannelData {
    pub description: String,
    pub candidate: String,
}

impl Default for ChannelData {
    fn default() -> ChannelData {
        ChannelData {
            description: "".to_owned(),
            candidate: "".to_owned(),
        }
    }
}

#[post("/json", format = "json", data = "<payload>")]
fn new(payload: Json<ChannelData>, state: rocket::State<Data>) -> JsonValue {
    let mut data = state.lock().expect("state locked");
    *data = payload.into_inner();
    json!({ "status": "ok" })
}

#[get("/json", format = "json")]
fn get(state: rocket::State<Data>) -> Option<Json<ChannelData>> {
    let data = state.lock().expect("state locked");
    Some(Json(data.clone()))
}

#[catch(404)]
fn not_found() -> JsonValue {
    json!({
        "status": "error",
        "reason": "Resource was not found."
    })
}

fn rocket() -> rocket::Rocket {
    rocket::ignite()
        .mount("/state", routes![new, get])
        .register(catchers![not_found])
        .manage(Mutex::new(ChannelData::default()))
        .attach(make_cors())
}

fn main() {
    rocket().launch();
}

#[cfg(test)]
mod test {
    use super::*;

    use rocket::http::{ContentType, Status};
    use rocket::local::Client;

    #[test]
    fn test_save_load() {
        let data = ChannelData {
            description: "v=0
            o=- 3294530454 0 IN IP4 127.0.0.1
            s=-
            t=0 0
            a=group:BUNDLE 0
            m=application 9 UDP/DTLS/SCTP webrtc-datachannel
            c=IN IP4 0.0.0.0
            a=ice-ufrag:ntAX
            a=ice-pwd:H59OKuJgItlvJZR5E78QYo
            a=ice-options:trickle
            a=mid:0
            a=setup:actpass
            a=dtls-id:1
            a=fingerprint:sha-256 72:0E:8D:8C:9F:A2:E4:40:E7:2E:23:EF:F6:E7:89:94:0F:6B:78:9A:36:61:43:2C:6A:45:30:62:CB:68:B3:73
            a=sctp-port:5000
            a=max-message-size:262144".to_owned(),
            candidate: "a=candidate:1 1 UDP 2122317823 10.0.1.83 55100 typ host".to_owned(),
        };
        println!("{:?}", data);
        let client = Client::new(rocket()).unwrap();

        let res = client
            .post("/state/json")
            .header(ContentType::JSON)
            .remote("127.0.0.1:8000".parse().unwrap())
            .body(serde_json::to_vec(&data).unwrap())
            .dispatch();
        assert_eq!(res.status(), Status::Ok);

        let mut res = client
            .get("/state/json")
            .header(ContentType::JSON)
            .dispatch();
        assert_eq!(res.status(), Status::Ok);

        let body = res.body().unwrap().into_string().unwrap();
        println!("{}", body);
    }
}
