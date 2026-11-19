#include "../src/json.hpp"
#include <cassert>
#include <iostream>
int main() {
    assert(tinyjson::escape("hi\n\"\\") == "hi\\n\\\"\\\\");
    std::string text;
    assert(tinyjson::completion_text(R"({"choices":[{"message":{"role":"assistant","content":"Привіт 🌍\\nHello"}}]})", text));
    assert(text == "Привіт 🌍\\nHello");
    assert(tinyjson::completion_text(R"({"choices":[{"message":{"content":"\u041f\u0440\u0438\u0432\u0456\u0442 \ud83d\ude4f"}}]})", text));
    assert(text == "Привіт 🙏");
    assert(!tinyjson::completion_text(R"({"error":{"message":"bad key"}})", text));
    assert(!tinyjson::completion_text(R"({"choices":[{"message":{"content":"\ud83d"}}]})", text));
    assert(tinyjson::ollama_chat_text(R"({"model":"qwen3:0.6b","message":{"role":"assistant","content":" Привіт, як справи?"},"done":true})", text));
    assert(text == " Привіт, як справи?");
    assert(tinyjson::ollama_chat_text(R"({"message":{"role":"assistant","content":"\u041f\u0440\u0438\u0432\u0435\u0442"}})", text));
    assert(text == "Привет");
    assert(!tinyjson::ollama_chat_text(R"({"error":"model not found"})", text));
    assert(!tinyjson::ollama_chat_text(R"({"message":{"content":null}})", text));
    std::cout << "JSON tests passed\n";
}
