#include "generators.h"
#include "softmax.h"
#include "search.h"
#include "beam_search_scorer.h"
#include <queue>

namespace Generators {

Search::Search(SearchParams params)
    : params_{params},
      sequences_{params.input_ids, params.batch_size, params.num_beams, params_.max_length} {

  auto batch_beam_size = params.BatchBeamSize();

  sequence_lengths_buffer_ = AllocateArray<int32_t>(batch_beam_size, &sequence_lengths_);

  size_t next_token_size = batch_beam_size * params_.vocab_size;
  next_token_scores_buffer_ = AllocateArray<ScoreType>(next_token_size, &next_token_scores_);
  memset(next_token_scores_.data(), 0, next_token_scores_.size_bytes());
}

GreedySearch::GreedySearch(SearchParams params)
    : Search(params) {
  next_tokens_buffer_ = AllocateArray<int32_t>(params.batch_size, &next_tokens_);
  memset(next_tokens_.data(), 0, next_tokens_.size_bytes());

  eos_seen_buffer_ = AllocateArray<bool>(params.batch_size, &eos_seen_);
  memset(eos_seen_.data(), 0, eos_seen_.size_bytes());
}

BeamSearch::BeamSearch(SearchParams params)
    : Search(params) {
  assert(params_.num_beams > 1);  // If 1, use GreedySearch
  beam_scorer_ = std::make_unique<BeamSearchScorer>(params_);
}

BeamSearch::~BeamSearch() = default;

void Search::SetLogits(std::span<const ScoreType> logits) {
  // Logits has shape (batch_size, input_length, vocab_size),
  // where input_length equals to parameters_->sequence_length for first subgraph call, and 1 for the remaining calls.

  auto batch_beam_size = params_.BatchBeamSize();
  auto input_length = logits.size() / (batch_beam_size * params_.vocab_size);
  assert(logits.size() % (batch_beam_size * params_.vocab_size) == 0);  // Should divide evenly

  // TODO: if input_length==1, use token scores directly

  // Get logits for the last token:
  //    next_token_logits = logits[:, -1, :], and the result shape is (batch_size, vocab_size)
  // When input_length == 1, use logits directly in SoftmaxCPU below so it only need for input_length > 1.
  const ScoreType* current_logits = logits.data() + (input_length - 1) * params_.vocab_size;
  for (int i = 0; i < batch_beam_size; i++) {
    std::span<const ScoreType> source(current_logits, params_.vocab_size);
    std::span<ScoreType> target = next_token_scores_.subspan(i * params_.vocab_size, params_.vocab_size);
    copy(source, target);
    current_logits += input_length * params_.vocab_size;

    log_softmax(target);
  }
}

std::span<int32_t> GreedySearch::GetNextTokens() {
  return next_tokens_;
}

std::span<int32_t> BeamSearch::GetNextTokens() {
  return beam_scorer_->GetNextTokens();
}

std::span<int32_t> BeamSearch::GetNextIndices() {
  return beam_scorer_->GetNextIndicesCPU();
}

int Search::GetSequenceLength() {
  return sequences_.GetSequenceLength();
}

void BeamSearch::SelectTopK() {
  auto beam_scores = beam_scorer_->GetNextScores();
  // Add beam score to next token scores. Corresponding python code is like:
  //    next_token_scores = next_token_scores + beam_scores[:, None].expand_as(next_token_scores)
  // TODO(tianleiwu): use thread pool to parallel
  int offset = 0;
  int batch_beam_index = 0;
  for (int i = 0; i < params_.batch_size; i++) {
    for (int j = 0; j < params_.num_beams; j++, batch_beam_index++) {
      for (int k = 0; k < params_.vocab_size; k++, offset++) {
        next_token_scores_[offset] += beam_scores[batch_beam_index];
      }
    }
  }

  // TODO: Write output scores?
  unsigned top_k = 2 * params_.num_beams;

  struct ScoreIndex {
    float score;
    int32_t index;

    bool operator<(const ScoreIndex &v) const { return score < v.score; }
  };

//  auto compare = [](const ScoreIndex& left, const ScoreIndex& right) { return left.score < right.score; };
  auto scores = std::make_unique<ScoreType[]>(top_k * params_.batch_size);
  auto indices = std::make_unique<int32_t[]>(top_k * params_.batch_size);
  auto tokens = std::make_unique<int32_t[]>(top_k * params_.batch_size);

  auto next_scores = std::span<float>(scores.get(), top_k * params_.batch_size);
  auto next_indices = std::span<int32_t>(indices.get(), top_k * params_.batch_size);
  auto next_tokens = std::span<int32_t>(tokens.get(), top_k * params_.batch_size);

  for (int batch_index = 0; batch_index < params_.batch_size; batch_index++) {
    std::priority_queue<ScoreIndex, std::vector<ScoreIndex>> queue;
    auto token_scores_sub = next_token_scores_.subspan(batch_index * params_.num_beams * params_.vocab_size, params_.num_beams * params_.vocab_size);
    for (int i = 0; i < token_scores_sub.size(); i++) {
      queue.push({token_scores_sub[i], i});
    }

    auto next_indices_sub = next_indices.subspan(top_k * batch_index, top_k);
    auto next_tokens_sub = next_tokens.subspan(top_k * batch_index, top_k);
    auto next_scores_sub = next_scores.subspan(top_k * batch_index, top_k);
    for (unsigned i = 0; i < top_k; i++) {
      auto v = queue.top();
      next_indices_sub[i] = v.index / params_.vocab_size;
      next_tokens_sub[i] = v.index % params_.vocab_size;
      next_scores_sub[i] = v.score;
      queue.pop();
    }
  }

#if 0
  DumpMemory("Next Scores", next_scores);
  DumpMemory("Next Tokens", next_tokens);
  DumpMemory("Next Indices", next_indices);
#endif

  beam_scorer_->Process(sequences_, next_scores, next_tokens, next_indices);
  next_tokens_ = beam_scorer_->GetNextTokens();

  AppendNextTokensToSequences();
}

void GreedySearch::SelectTop1() {
  auto next_token_scores = next_token_scores_.data();
  // next_tokens = torch.argmax(scores, dim=-1)
  for (size_t batch_id = 0; batch_id < params_.batch_size; batch_id++) {

    // If this batch entry has already seen the EOS token, append the pad token
    if (eos_seen_[batch_id]) {
      next_tokens_[batch_id] = params_.pad_token_id;
      continue;
    }

    int32_t best_token = 0;
    ScoreType best_score = next_token_scores[0];
    for (int32_t token = 1; token < params_.vocab_size; token++) {
      if (next_token_scores[token] > best_score) {
        best_score = next_token_scores[token];
        best_token = token;
      }
    }
    next_tokens_[batch_id] = best_token;
    next_token_scores += params_.vocab_size;

    if (best_token == params_.eos_token_id) {
      eos_seen_[batch_id] = true;
      if (--not_done_count_==0)
        done_=true;
    }
  }

  AppendNextTokensToSequences();
}

void GreedySearch::AppendNextTokensToSequences() {
  sequences_.AppendNextTokenToSequences(next_tokens_);

  if (sequences_.GetSequenceLength() == params_.max_length)
    done_ = true;
}

void BeamSearch::AppendNextTokensToSequences() {
  sequences_.AppendNextTokenToSequences(beam_scorer_->GetNextIndicesCPU(), beam_scorer_->GetNextTokens());

  if (sequences_.GetSequenceLength() == params_.max_length)
    done_ = true;
}

void BeamSearch::Finalize(size_t num_return_sequences, std::span<int32_t> output, std::span<float> sequence_scores) {
  beam_scorer_->Finalize(sequences_, num_return_sequences, output, sequence_scores);
}

#if 0
// Not needed, for greedy can just grab the output sequence directly?
void GreedySearch::Finalize(size_t num_return_sequences, std::span<int32_t> output, std::span<float> sequence_scores) {
  auto shape=output_sequences_->GetTensorTypeAndShapeInfo()->GetShape();
  size_t shape_count = std::accumulate(shape.begin(), shape.end(), 1LL, std::multiplies<int64_t>());

  // Copy the sequences to output
  std::span<int32_t> output{ output_sequences_->GetTensorMutableData<int32_t>(), shape_count};
  for (int batch_id = 0; batch_id < params_.batch_size; ++batch_id) {
    auto batch_output = output.subspan(
        static_cast<size_t>(batch_id) * params_.max_length,
        params_.max_length);
    std::span<const int32_t> sequence_source = sequences_.GetSequence(batch_id);
    std::copy(sequence_source, batch_output);
  }
}
#endif

std::span<ScoreType> Search::GetScores(int batch_beam_index) {
  assert(batch_beam_index >= 0 && batch_beam_index < params_.BatchBeamSize());
  return next_token_scores_.subspan(batch_beam_index * params_.vocab_size, params_.vocab_size);
}

namespace Processors {

void MinLength(Search& search, int min_length) {
  if (search.sequences_.GetSequenceLength() >= min_length)
    return;

  const int batch_beam_size = search.params_.BatchBeamSize();
  for (int i = 0; i < batch_beam_size; i++) {
    std::span<ScoreType> beam_token_scores = search.GetScores(i);
    beam_token_scores[search.params_.eos_token_id] = std::numeric_limits<ScoreType>::lowest();
  }
}

void RepetitionPenalty(Search& search, ScoreType penalty) {
  const int batch_beam_size = search.params_.BatchBeamSize();
  for (int i = 0; i < batch_beam_size; i++) {
    std::span<ScoreType> beam_token_scores = search.GetScores(i);
    std::span<const int32_t> sequence = search.sequences_.GetSequence(i);

    // Find unique word IDs in sequence.
    std::unordered_set<int32_t> unique_word_ids;
    for (const auto& word_id : sequence) {
      unique_word_ids.insert(word_id);
    }

    for (const int32_t word_id : unique_word_ids) {
      ScoreType score = beam_token_scores[word_id];

      // If score < 0, then repetition penalty > 1.0 has to multiplied to reduce the previous token probability,
      // This assumes that scores are either positive (like ctrl) or negative (like GPT-2), but not a mixture.
      beam_token_scores[word_id] = (score < 0 ? score * penalty : score / penalty);
    }
  }
}

}  // namespace Processors

}  // namespace Generators