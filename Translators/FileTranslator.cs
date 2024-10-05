// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class FileTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundRead = false;
			bool foundReadBinary = false;
			bool foundReadLines = false;
			bool foundWriteLine = false;
			bool foundWrite = false;
			bool foundWriteBinary = false;
			bool foundAppend = false;
			bool foundAppendBinary = false;
			bool foundDelete = false;
			bool foundExists = false;

			// Support for random method names to avoid conflicts.
			string fileReadSuffix = "";
			string fileReadBinarySuffix = "";
			string fileReadLinesSuffix = "";
			string fileWriteLineSuffix = "";
			string fileWriteSuffix = "";
			string fileWriteBinarySuffix = "";
			string fileAppendSuffix = "";
			string fileAppendBinarySuffix = "";
			string fileDeleteSuffix = "";
			string fileExistsSuffix = "";

			// Define regex patterns for File.Read, File.ReadBinary, File.ReadLines, File.WriteLine, File.Write, File.WriteBinary, File.Append, File.AppendBinary, File.Delete and File.Exists.
			string readPattern = @"File\.(?i)read\(([^;]+)\)";
			string readBinaryPattern = @"File\.(?i)readBinary\(([^;]+)\)";
			string readLinesPattern = @"File\.(?i)readLines\(([^;]+)\)";
			string writeLinePattern = @"File\.(?i)writeLine\(([^;]+),\s*([^;]+)\)";
			string writePattern = @"File\.(?i)write\(([^;]+),\s*([^;]+)\)";
			string writeBinaryPattern = @"File\.(?i)writeBinary\(([^;]+),\s*([^;]+)\)";
			string appendPattern = @"File\.(?i)append\(([^;]+),\s*([^;]+)\)";
			string appendBinaryPattern = @"File\.(?i)appendBinary\(([^;]+),\s*([^;]+)\)";
			string deletePattern = @"File\.(?i)delete\(([^;]+)\)";
			string existsPattern = @"File\.(?i)exists\(([^;]+)\)";

			// Replace File.read with fileRead and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileRead("))
				{
					fileReadSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				return $"fileRead{fileReadSuffix}({fileName})";
			});

			// Replace File.readBinary with fileReadBinary and track if found.
			source = Regex.Replace(source, readBinaryPattern, match =>
			{
				foundReadBinary = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileReadBinary("))
				{
					fileReadBinarySuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				return $"fileReadBinary{fileReadBinarySuffix}({fileName})";
			});

			// Replace File.readLines with fileReadLines and track if found.
			source = Regex.Replace(source, readLinesPattern, match =>
			{
				foundReadLines = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileReadLines("))
				{
					fileReadLinesSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				return $"fileReadLines{fileReadLinesSuffix}({fileName})";
			});

			// Replace File.writeLine with fileWriteLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileWriteLine("))
				{
					fileWriteLineSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWriteLine{fileWriteLineSuffix}({fileName}, {text})";
			});

			// Replace File.write with fileWrite and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileWrite("))
				{
					fileWriteSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWrite{fileWriteSuffix}({fileName}, {text})";
			});

			// Replace File.writeBinary with fileWriteBinary and track if found.
			source = Regex.Replace(source, writeBinaryPattern, match =>
			{
				foundWriteBinary = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileWriteBinary("))
				{
					fileWriteBinarySuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				string data = match.Groups[2].Value;
				return $"fileWriteBinary{fileWriteSuffix}({fileName}, {data})";
			});

			// Replace File.append with fileAppend and track if found.
			source = Regex.Replace(source, appendPattern, match =>
			{
				foundAppend = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileAppend("))
				{
					fileAppendSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileAppend{fileAppendSuffix}({fileName}, {text})";
			});

			// Replace File.appendBinary with fileAppendBinary and track if found.
			source = Regex.Replace(source, appendBinaryPattern, match =>
			{
				foundAppendBinary = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileAppendBinary("))
				{
					fileAppendBinarySuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				string data = match.Groups[2].Value;
				return $"fileAppendBinary{fileAppendBinarySuffix}({fileName}, {data})";
			});

			// Replace File.delete with fileDelete and track if found.
			source = Regex.Replace(source, deletePattern, match =>
			{
				foundDelete = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileDelete("))
				{
					fileDeleteSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				return $"fileDelete{fileDeleteSuffix}({fileName})";
			});

			// Replace File.exists with fileExists and track if found.
			source = Regex.Replace(source, existsPattern, match =>
			{
				foundExists = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("fileExists("))
				{
					fileExistsSuffix = GetRandomMethodIdentifier();
				}
				string fileName = match.Groups[1].Value;
				return $"fileExists{fileExistsSuffix}({fileName})";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Append fileRead method if it was found.
			if (foundRead)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"std::string fileRead{fileReadSuffix}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\tstd::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());");
				methods.AppendLine("\treturn content;");
				methods.AppendLine("}\n");
			}

			// Append fileReadBinary method if it was found.
			if (foundReadBinary)
			{
				source = AddInclude(source, "fstream");
				source = AddInclude(source, "vector");

				methods.AppendLine($"std::vector<char> fileReadBinary{fileReadBinarySuffix}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName, std::ios::binary);");
				methods.AppendLine("\tif (!file.is_open())");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\treturn {};");
				methods.AppendLine("\t}");
				methods.AppendLine("\tfile.seekg(0, std::ios::end);");
				methods.AppendLine("\tstd::streamsize size = file.tellg();");
				methods.AppendLine("\tfile.seekg(0, std::ios::beg);");
				methods.AppendLine("\tstd::vector<char> buffer(size);");
				methods.AppendLine("\tfile.read(buffer.data(), size);");
				methods.AppendLine("\treturn buffer;");
				methods.AppendLine("}\n");
			}

			// Append fileReadLines method if it was found.
			if (foundReadLines)
			{
				source = AddInclude(source, "fstream");
				source = AddInclude(source, "vector");

				methods.AppendLine($"std::vector<std::string> fileReadLines{fileReadLinesSuffix}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\tstd::vector<std::string> lines;");
				methods.AppendLine("\tstd::string line;");
				methods.AppendLine("\twhile (std::getline(file, line))");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tlines.push_back(line);");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn lines;");
				methods.AppendLine("}\n");
			}

			// Append fileWriteLine method if it was found.
			if (foundWriteLine)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"void fileWriteLine{fileWriteLineSuffix}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				methods.AppendLine("\tfile << text << std::endl;");
				methods.AppendLine("}\n");
			}

			// Append fileWrite method if it was found.
			if (foundWrite)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"void fileWrite{fileWriteSuffix}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios::out);");
				methods.AppendLine("\tif (file.is_open())");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tfile << text;");
				methods.AppendLine("\t}");
				methods.AppendLine("}\n");
			}

			// Append fileWriteBinary method if it was found.
			if (foundWriteBinary)
			{
				source = AddInclude(source, "fstream");
				source = AddInclude(source, "vector");

				methods.AppendLine($"void fileWriteBinary{fileWriteBinarySuffix}(const std::string& fileName, const std::vector<char>& data)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios::binary);");
				methods.AppendLine("\tif (file.is_open())");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tfile.write(data.data(), data.size());");
				methods.AppendLine("\t}");
				methods.AppendLine("}\n");
			}

			// Append fileAppend method if it was found.
			if (foundAppend)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"void fileAppend{fileAppendSuffix}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				methods.AppendLine("\tif (file.is_open())");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tfile << text;");
				methods.AppendLine("\t}");
				methods.AppendLine("}\n");
			}

			// Append fileAppendBinary method if it was found.
			if (foundAppendBinary)
			{
				source = AddInclude(source, "fstream");
				source = AddInclude(source, "vector");

				methods.AppendLine($"void fileAppendBinary{fileAppendBinarySuffix}(const std::string& fileName, const std::vector<char>& data)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app | std::ios::binary);");
				methods.AppendLine("\tif (file.is_open())");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tfile.write(data.data(), data.size());");
				methods.AppendLine("\t}");
				methods.AppendLine("}\n");
			}

			// Append fileDelete method if it was found.
			if (foundDelete)
			{
				source = AddInclude(source, "cstdio");

				methods.AppendLine($"void fileDelete{fileDeleteSuffix}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::remove(fileName.c_str());");
				methods.AppendLine("}\n");
			}

			// Append fileExists method if it was found.
			if (foundExists)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"bool fileExists{fileExistsSuffix}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\treturn file.good();");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
