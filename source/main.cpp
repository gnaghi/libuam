#include "compiler_iface.h"
#include <getopt.h>

static const uint32_t SPIRV_MAGIC = 0x07230203;

static int usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [options] file\n"
		"Options:\n"
		"  -o, --out=<file>   Specifies the output deko3d shader module file (.dksh)\n"
		"  -r, --raw=<file>   Specifies the file to which output raw Maxwell bytecode\n"
		"  -t, --tgsi=<file>  Specifies the file to which output intermediary TGSI code\n"
		"  -s, --stage=<name> Specifies the pipeline stage of the shader\n"
		"                     (vert, tess_ctrl, tess_eval, geom, frag, comp)\n"
		"  -i, --input-format=<fmt>  Input format: glsl (default) or spirv\n"
		"                     glsl: Mesa frontend (default)\n"
		"                     spirv: direct SPIR-V input\n"
		"                     Auto-detected from file content if not specified\n"
		"  -v, --version      Displays version information\n"
		, prog);
	return EXIT_FAILURE;
}

int main(int argc, char* argv[])
{
	const char *inFile = nullptr, *outFile = nullptr, *rawFile = nullptr,
	           *tgsiFile = nullptr, *stageName = nullptr, *inputFormat = nullptr;

	static struct option long_options[] =
	{
		{ "out",          required_argument, NULL, 'o' },
		{ "raw",          required_argument, NULL, 'r' },
		{ "tgsi",         required_argument, NULL, 't' },
		{ "stage",        required_argument, NULL, 's' },
		{ "input-format", required_argument, NULL, 'i' },
		{ "help",         no_argument,       NULL, '?' },
		{ "version",      no_argument,       NULL, 'v' },
		{ NULL, 0, NULL, 0 }
	};

	int opt, optidx = 0;
	while ((opt = getopt_long(argc, argv, "o:r:t:s:i:?v", long_options, &optidx)) != -1)
	{
		switch (opt)
		{
			case 'o': outFile = optarg; break;
			case 'r': rawFile = optarg; break;
			case 't': tgsiFile = optarg; break;
			case 's': stageName = optarg; break;
			case 'i': inputFormat = optarg; break;
			case '?': usage(argv[0]); return EXIT_SUCCESS;
			case 'v': printf("%s - Built on %s %s\n", PACKAGE_STRING, __DATE__, __TIME__); return EXIT_SUCCESS;
			default:  return usage(argv[0]);
		}
	}

	if ((argc-optind) != 1)
		return usage(argv[0]);
	inFile = argv[optind];

	if (!stageName)
	{
		fprintf(stderr, "Missing pipeline stage argument (--stage)\n");
		return EXIT_FAILURE;
	}

	if (!outFile && !rawFile && !tgsiFile)
	{
		fprintf(stderr, "No output file specified\n");
		return EXIT_FAILURE;
	}

	pipeline_stage stage;
	if (0) ((void)0);
#define TEST_STAGE(_str,_val) else if (strcmp(stageName,(_str))==0) stage = (_val)
	TEST_STAGE("vert", pipeline_stage_vertex);
	TEST_STAGE("tess_ctrl", pipeline_stage_tess_ctrl);
	TEST_STAGE("tess_eval", pipeline_stage_tess_eval);
	TEST_STAGE("geom", pipeline_stage_geometry);
	TEST_STAGE("frag", pipeline_stage_fragment);
	TEST_STAGE("comp", pipeline_stage_compute);
#undef TEST_STAGE
	else
	{
		fprintf(stderr, "Unrecognized pipeline stage: `%s'\n", stageName);
		return EXIT_FAILURE;
	}

	FILE* fin = fopen(inFile, "rb");
	if (!fin)
	{
		fprintf(stderr, "Could not open input file: %s\n", inFile);
		return EXIT_FAILURE;
	}

	fseek(fin, 0, SEEK_END);
	long fsize = ftell(fin);
	rewind(fin);

	char* fileData = new char[fsize+1];
	fread(fileData, 1, fsize, fin);
	fclose(fin);
	fileData[fsize] = 0;

	// Determine input format: auto-detect SPIR-V magic if not specified
	// 0 = glsl (mesa), 1 = spirv
	int inputMode = 0;
	if (inputFormat)
	{
		if (strcmp(inputFormat, "spirv") == 0)
			inputMode = 1;
		else if (strcmp(inputFormat, "glsl") == 0)
			inputMode = 0;
		else
		{
			fprintf(stderr, "Unrecognized input format: `%s' (use glsl or spirv)\n", inputFormat);
			delete[] fileData;
			return EXIT_FAILURE;
		}
	}
	else if (fsize >= 4)
	{
		uint32_t magic;
		memcpy(&magic, fileData, sizeof(magic));
		if (magic == SPIRV_MAGIC)
			inputMode = 1;
	}

	DekoCompiler compiler{stage};
	bool rc;

	if (inputMode == 1)
	{
		if (fsize < 20 || (fsize % 4) != 0)
		{
			fprintf(stderr, "Invalid SPIR-V binary (too small or not word-aligned)\n");
			delete[] fileData;
			return EXIT_FAILURE;
		}
		rc = compiler.CompileSpirv(reinterpret_cast<const uint32_t*>(fileData), fsize / 4);
	}
	else
	{
		rc = compiler.CompileGlsl(fileData);
	}

	delete[] fileData;

	if (!rc)
	{
		const char *errLog = compiler.GetErrorLog();
		if (errLog && errLog[0])
			fprintf(stderr, "Compilation failed:\n%s\n", errLog);
		else
			fprintf(stderr, "Compilation failed (no error log)\n");
		return EXIT_FAILURE;
	}

	/* Print uniform metadata if present */
	int numUniforms = compiler.GetNumUniforms();
	int numSamplers = compiler.GetNumSamplers();
	if (numUniforms > 0 || numSamplers > 0)
	{
		printf("--- Metadata (uniforms: %d, samplers: %d, constbuf: %u bytes, remapped: %s) ---\n",
			numUniforms, numSamplers, compiler.GetConstbufSize(),
			compiler.IsConstbufRemapped() ? "YES (c[0]->c[1])" : "no");
		for (int i = 0; i < numUniforms; i++)
		{
			const glsl_uniform_info_t *u = compiler.GetUniformInfo(i);
			if (!u) continue;
			const char *typeNames[] = {"uint","int","float","?","?","?","bool","sampler"};
			const char *typeName = (u->base_type < 8) ? typeNames[u->base_type] : "?";
			printf("  uniform[%d] %s: %s", i, u->name, typeName);
			if (u->matrix_columns > 1)
				printf("mat%dx%d", u->matrix_columns, u->vector_elements);
			else if (u->vector_elements > 1)
				printf("vec%d", u->vector_elements);
			printf(" offset=%u size=%u", u->offset, u->size_bytes);
			if (u->array_elements > 0)
				printf(" array[%u]", u->array_elements);
			printf("\n");
		}
		for (int i = 0; i < numSamplers; i++)
		{
			const glsl_sampler_info_t *s = compiler.GetSamplerInfo(i);
			if (!s) continue;
			printf("  sampler[%d] %s: binding=%d type=%s\n",
				i, s->name, s->binding,
				s->type == 1 ? "samplerCube" : "sampler2D");
		}
		printf("---\n");
	}

	if (outFile)
		compiler.OutputDksh(outFile);

	if (rawFile)
		compiler.OutputRawCode(rawFile);

	if (tgsiFile && inputMode == 0)
		compiler.OutputTgsi(tgsiFile);

	return EXIT_SUCCESS;
}
