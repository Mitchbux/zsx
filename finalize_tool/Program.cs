using System;
using System.IO;

public class Finalize
{
    public static void Main(string[] args)
    {
        if (args.Length > 1)
        {
            var txt = File.ReadAllLines(args[0]);
            var max = txt.Length - 2;
            var skip = 2;
            Console.WriteLine(args[1] + " = function() {");
            foreach (var line in txt)
            {
                if (skip > 0) skip--;
                else Console.WriteLine(line);
                max--;
                if (max == 0) break;
            }
            Console.WriteLine("};");
        }
        else
            Console.WriteLine("Usage : Finalize.exe Filename functionName");
    }
}
