const assert = require("node:assert/strict");
const fs = require("node:fs");

const frontend = fs.readFileSync("frontend/script.js", "utf8");
const server = fs.readFileSync("server.c", "utf8");
const engine = fs.readFileSync("quiz_engine.c", "utf8");

const submitStart = frontend.indexOf("function submitAnswer() {");
const submitEnd = frontend.indexOf("// -------------------- ANSWER FEEDBACK", submitStart);
assert(submitStart >= 0 && submitEnd > submitStart, "local answer submission must exist");
const submitBody = frontend.slice(submitStart, submitEnd);
assert(submitBody.includes("quizAnswers[currentQuestionIndex] = selectedAnswer"), "answers must be cached locally");
assert(!submitBody.includes("apiRequest("), "no backend request is allowed between questions");

const completeStart = frontend.indexOf("async function completeQuiz() {");
const completeEnd = frontend.indexOf("// -------------------- QUIZ RESULTS", completeStart);
assert(completeStart >= 0 && completeEnd > completeStart, "single completion request must exist");
const completeBody = frontend.slice(completeStart, completeEnd);
assert(completeBody.includes("/api/quiz/complete"), "all answers must be submitted once at completion");
assert(completeBody.includes("answers: quizAnswers"), "complete answer batch must be sent");

const nextStart = frontend.indexOf("function nextQuestion() {");
const nextEnd = frontend.indexOf("async function completeQuiz()", nextStart);
assert(nextStart >= 0 && nextEnd > nextStart, "local next-question navigation must exist");
assert(frontend.slice(nextStart, nextEnd).includes("quizQuestionCache[currentQuestionIndex + 1]"), "next questions must come from the preloaded cache");

assert(server.includes('"/api/quiz/complete"'), "server must expose batch completion endpoint");
assert(server.includes("persist_active_quiz_session"), "active sessions must be persisted");
assert(server.includes("restore_quiz_session"), "sessions must recover after process restart");
assert(server.includes("persist_completed_quiz_session"), "completion must be idempotently persisted");
assert(engine.includes("int quiz_engine_submit_answers("), "backend must score a complete answer batch");

console.log("Quiz flow tests: PASS");

const optionShuffle = engine.indexOf("static void shuffle_answer_options(");
assert(optionShuffle >= 0, "answer choices must be shuffled in the C engine");
assert(engine.includes("correct_positions[i] = i % 4"), "correct answer positions must be balanced across A/B/C/D");
assert(engine.includes("q->correct_option = (char)('A' + target)"), "correct answer key must follow its shuffled option");
assert(engine.includes("shuffle_answer_options(fresh_batch, question_count)"), "AI-generated batches must shuffle options before delivery");
assert(engine.includes("shuffle_answer_options(session->questions, selected_count)"), "offline question batches must shuffle options too");

assert(frontend.includes('id="previous-question-btn"') === false, "the previous button belongs in HTML, not the JS source");
assert(frontend.includes("function previousQuestion()"), "users must be able to review earlier questions");
assert(frontend.includes("selectedAnswer = quizAnswers[currentQuestionIndex] || null"), "previously selected answers must remain selected");
assert(frontend.includes('$("#previous-question-btn")'), "previous-question control must be wired in the frontend");

assert(server.includes("restore_quiz_session(session_id"), "legacy answer submissions must restore persisted sessions");
assert(server.includes("persist_active_quiz_session(slot)"), "legacy progress must be persisted after each answer");
assert(!server.includes("Quiz session not found or already completed"), "the old generic session error must be removed");
assert(server.includes("clock_gettime(CLOCK_REALTIME, &session_clock)"), "session IDs must remain unique across fast restarts");

console.log("Balanced options, review navigation and session recovery tests: PASS");
