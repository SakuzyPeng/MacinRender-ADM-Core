//! Scene stream rendering (PCM consistency 5.4): the renderer behind each element generation,
//! chosen and created from the stream configuration (5.4 A), the stream engine that queues,
//! renders and hands out the frames (5.4 B), and the output session a device pulls them through
//! (5.4 C).
pub mod output;
pub mod renderer;
pub mod stream;
