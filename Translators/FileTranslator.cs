// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class FileTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			// Define regex patterns for File.Read, File.ReadLines, File.WriteLine, File.Write, File.Append, File.Delete, and File.Exists.
			string readPattern = @"File\.read\(([^;]+)\);";
			string readLinesPattern = @"File\.readLines\(([^;]+)\);";
			string writeLinePattern = @"File\.writeLine\(([^;]+),\s*([^;]+)\);";
			string writePattern = @"File\.write\(([^;]+),\s*([^;]+)\);";
			string appendPattern = @"File\.append\(([^;]+),\s*([^;]+)\);";
			string deletePattern = @"File\.delete\(([^;]+)\);";
			string existsPattern = @"File\.exists\(([^;]+)\);";

			bool foundRead = false;
			bool foundReadLines = false;
			bool foundWriteLine = false;
			bool foundWrite = false;
			bool foundAppend = false;
			bool foundDelete = false;
			bool foundExists = false;

			// Replace File.read and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				string fileName = match.Groups[1].Value;
				return $"fileRead({fileName});";
			});

			// Replace File.readLines and track if found.
			source = Regex.Replace(source, readLinesPattern, match =>
			{
				foundReadLines = true;
				string fileName = match.Groups[1].Value;
				return $"fileReadLines({fileName});";
			});

			// Replace File.writeLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWriteLine({fileName}, {text});";
			});

			// Replace File.write and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWrite({fileName}, {text});";
			});

			// Replace File.append and track if found.
			source = Regex.Replace(source, appendPattern, match =>
			{
				foundAppend = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileAppend({fileName}, {text});";
			});

			// Replace File.delete and track if found.
			source = Regex.Replace(source, deletePattern, match =>
			{
				foundDelete = true;
				string fileName = match.Groups[1].Value;
				return $"fileDelete({fileName});";
			});

			// Replace File.exists and track if found.
			source = Regex.Replace(source, existsPattern, match =>
			{
				foundExists = true;
				string fileName = match.Groups[1].Value;
				return $"fileExists({fileName});";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder header = new StringBuilder();

			// Append fileRead method if it was found.
			if (foundRead)
			{
				header.AppendLine("std::string fileRead(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\tstd::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());");
				header.AppendLine("\treturn content;");
				header.AppendLine("}\n");
			}

			// Append fileReadLines method if it was found.
			if (foundReadLines)
			{
				header.AppendLine("std::vector<std::string> fileReadLines(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\tstd::vector<std::string> lines;");
				header.AppendLine("\tstd::string line;");
				header.AppendLine("\twhile (std::getline(file, line))");
				header.AppendLine("\t{");
				header.AppendLine("\t\tlines.push_back(line);");
				header.AppendLine("\t}");
				header.AppendLine("\treturn lines;");
				header.AppendLine("}\n");
			}

			// Append fileWriteLine method if it was found.
			if (foundWriteLine)
			{
				header.AppendLine("void fileWriteLine(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				header.AppendLine("\tfile << text << std::endl;");
				header.AppendLine("}\n");
			}

			// Append fileWrite method if it was found.
			if (foundWrite)
			{
				header.AppendLine("void fileWrite(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName);");
				header.AppendLine("\tfile << text;");
				header.AppendLine("}\n");
			}

			// Append fileAppend method if it was found.
			if (foundAppend)
			{
				header.AppendLine("void fileAppend(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				header.AppendLine("\tfile << text;");
				header.AppendLine("}\n");
			}

			// Append fileDelete method if it was found.
			if (foundDelete)
			{
				if (!source.Contains("#include <cstdio>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <cstdio>\n\n" + source;
					}
					else
					{
						source = "#include <cstdio>\n" + source;
					}
				}
				header.AppendLine("void fileDelete(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::remove(fileName.c_str());");
				header.AppendLine("}\n");
			}

			// Append fileExists method if it was found.
			if (foundExists)
			{
				if (!source.Contains("#include <fstream>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <fstream>\n\n" + source;
					}
					else
					{
						source = "#include <fstream>\n" + source;
					}
				}
				header.AppendLine("bool fileExists(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\treturn file.good();");
				header.AppendLine("}\n");
			}

			source = AddHeader(source, header.ToString());

			return source;
		}
	}
}
