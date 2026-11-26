# This README covers each version and what each solves. 

## TODO
  - focus on pruning techniques, main difference between different BnB algorithms

## Versions
### v2.0 
**Models** <br>
ChatGPT Astra 6 (Ultra), Claude Sonnet 5.5 (High) <br>
**Description** <br>
Starting from v2.x we begin working towards experimenting the best ways to educate the LLM's
specific algorithmic techniques to improve the performance of their created SOP solver's as a result
of the programs created under v1.5. Each change will be expressed incrementally one change at a time.
For this first change we try to modify the programs created in v1.5 and adjust them such that they 
integrate history domination without telling them how we have optimized utilizing history 
domination in parallel. Earlier test on simply copying adding the published papers resulted in 
regressions. As a result we will start integrating further changes one step at a time. 

### v1.5
11/8/2026 - 11/22/26<br>
**Models** <br>
ChatGPT Astra 6 (Ultra), Claude Sonnet 5.5 (High), Gemini Argon 4 (High) <br>
**Description** <br>
Integrated the two prompts from v1.4 into one complete prompt to propertly document and have an easier time 
reading the code and understanding the implemented algorithms. <br>

### v1.4
10/23/2026 - 11/8/2026 <br>
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
