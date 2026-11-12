# This README covers each version and what each solves. 

## TODO

- [x] specify c++17
- [x] list references used!
- [ ] algorithm info
  - [ ] if existing algorithms used, list those specific references
  - [ ] otherwise, explain new algorithms
  - focus on pruning techniques, main difference between different BnB algorithms

## Versions
### v1.4
10/23/2026 - current <br>
**Models** <br>
ChatGPT Astra 6 (Extra High), Claude Sonnet 5.5 (High), Gemini Argon 4 (High) <br>
**Description** <br> 
Additional modifications focused on having LLM's make full production/research ready projects. For 
more modular and sophisticated code, contiuned to be generated through UI. Additional prompt 
for referring to references on techniques being used, may have to refine towards focusing on BB 
algorithms. As current iteration is messy, will have to adjust next prompt such that they reference
during creation of the algorithm not after. 

### v1.3
10/19/2026 - 10/23/2026 <br>
**Models** <br>
ChatGPT Astra 6 (Medium), Claude Sonnet 5.5 (Medium), Gemini Argon 4 (Medium) <br>
**Description** <br>
Same as v1.2 but tested with adding additional context (research papers) on known ways to develop a solver <br>

### v1.2
10/19/2026 - 10/23/2026 <br>
**Models** <br>
ChatGPT Astra 6 (Medium), Claude Sonnet 5.5 (Medium), Gemini Argon 4 (Medium) <br>
Astra - 18m 48s <br>
Sonnet - ~19m <br>
Argon - ~10m <br>
**Description** <br>
Developed a more refined prompt derived from v1.1 prompt. Introduce HW and OS the program will be 
running on to create a parallel sop solver solution. <br>

### v1.1
10/12/2026 - 10/19/2026 <br>
**Models** <br>
ChatGPT Astra 6, Claude Sonnet 5.5, Gemini Flash 3.5, Grok 4.5, MetaAI Musespark 1.1, 
Co-Pilot GPT-5, Deepseek Flash v4.1 <br>
**Description** <br>
Query each LLM with a set prompt to **create a program** the best possible SOP solver they can. Run
each of their solutions on research machines and record the results against our SOP solver. <br>

### v1.0
10/5/2026 - 10/12/2026 <br>
**Models** <br>
ChatGPT Astra 6, Claude Sonnet 5.5, Gemini Flash 3.5, Grok 4.5, MetaAI Musespark 1.1, 
Co-Pilot GPT-5, Deepseek Flash v4.1  <br>
**Description** <br>
Gather a handleful of known LLM's and query them **to compute** the optimal solution for 
SOP instances. <br> 
