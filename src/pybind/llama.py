﻿import ort_generators as og
import numpy as np
from transformers import LlamaTokenizer

# Generate input tokens from the text prompt
tokenizer = LlamaTokenizer.from_pretrained('meta-llama/Llama-2-7b-hf')

print("Loading model...")
model=og.Llama("../../test_models/llama2-7b-fp32-cpu/Llama-2-7b-hf_decoder_merged_model_fp32_opt.onnx")
print("Model loaded")

# Keep asking for input prompts in an loop
while True:
    text = input("Input:")
    input_tokens = tokenizer.encode(text, return_tensors='np')

    params=og.SearchParams()
    params.max_length = 128
    params.batch_size = input_tokens.shape[0]
    params.sequence_length = input_tokens.shape[1]
    params.input_ids = input_tokens
    params.vocab_size = model.GetVocabSize()
    params.eos_token_id = tokenizer.eos_token_id
    params.pad_token_id = tokenizer.pad_token_id if tokenizer.pad_token_id is not None else params.eos_token_id

    search=og.GreedySearch(params)
    model.CreateInputs(search.GetSequenceLengths(), params)

    print("Output:")

    print(text, end='', flush=True)
    while not search.IsDone():
        model.Run(search.GetNextTokens(), search.GetSequenceLength())
        search.SetLogits(model.GetLogits())

        # search.Apply_MinLength(1)
        # search.Apply_RepetitionPenalty(1.0)

        search.SampleTopP(0.9, 0.6)

        # Print each token as we compute it, we have to do some work to get newlines & spaces to appear properly:
        word=tokenizer.convert_ids_to_tokens([search.GetNextTokens()[0]])[0]
        if word=='<0x0A>':
          word = '\n'
        if word.startswith('▁'):
          word = ' ' + word[1:]
        print(word, end='', flush=True)

    print()
    print()