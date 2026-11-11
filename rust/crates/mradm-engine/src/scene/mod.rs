//! Scene stream rendering: the renderer behind each element generation,
//! chosen and created from the stream configuration, the stream engine that queues,
//! renders and hands out the frames, and the output session a device pulls them through.
pub mod output;
pub mod renderer;
pub mod stream;
